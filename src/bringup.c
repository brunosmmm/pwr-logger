#include "bringup.h"
#include "app_config.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/logging/log.h>

#include <nrfx.h>
#include <nrfx_saadc.h>

LOG_MODULE_REGISTER(bringup, LOG_LEVEL_INF);

#define INA228_ADDR      0x40U
#define INA228_REG_MANID 0x3EU /* expect 0x5449 = "TI" */
#define INA228_REG_DEVID 0x3FU /* expect 0x228x */
#define SD_DISK          "SD"

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct device *const i2c_bus = DEVICE_DT_GET(DT_NODELABEL(i2c0));
static bool saadc_ready;

static void probe_ina228(void)
{
	uint8_t reg = INA228_REG_MANID;
	uint8_t buf[2] = {0};

	if (i2c_write_read(i2c_bus, INA228_ADDR, &reg, 1, buf, sizeof(buf)) != 0) {
		LOG_WRN("INA228: no ACK at 0x%02x -- not wired?", INA228_ADDR);
		return;
	}
	uint16_t manid = (uint16_t)((buf[0] << 8) | buf[1]);
	uint16_t devid = 0;

	reg = INA228_REG_DEVID;
	if (i2c_write_read(i2c_bus, INA228_ADDR, &reg, 1, buf, sizeof(buf)) == 0) {
		devid = (uint16_t)((buf[0] << 8) | buf[1]);
	}
	LOG_INF("INA228 @0x%02x: MANID=0x%04x DEVID=0x%04x%s", INA228_ADDR, manid,
		devid, (manid == 0x5449) ? " (TI OK)" : "");
}

static void probe_sd(void)
{
	uint32_t sectors = 0;
	uint32_t ssize = 0;

	if (disk_access_init(SD_DISK) != 0) {
		LOG_WRN("SD: no card / init failed -- not wired?");
		return;
	}
	disk_access_ioctl(SD_DISK, DISK_IOCTL_GET_SECTOR_COUNT, &sectors);
	disk_access_ioctl(SD_DISK, DISK_IOCTL_GET_SECTOR_SIZE, &ssize);
	LOG_INF("SD: %u sectors x %u B = %llu MB", sectors, ssize,
		((uint64_t)sectors * ssize) / (1024ULL * 1024ULL));
}

static int saadc_setup(void)
{
	nrfx_saadc_channel_t ch =
		NRFX_SAADC_DEFAULT_CHANNEL_SE(NRF_SAADC_INPUT_AIN7, 0);

	/* INA240 0..3.3 V output -> 0..3.6 V FS (internal 0.6 V ref, gain 1/6). */
	ch.channel_config.gain = NRF_SAADC_GAIN1_6;
	ch.channel_config.reference = NRF_SAADC_REFERENCE_INTERNAL;

	if (nrfx_saadc_init(NRFX_SAADC_DEFAULT_CONFIG_IRQ_PRIORITY) != 0) {
		return -1;
	}
	if (nrfx_saadc_channel_config(&ch) != 0) {
		return -1;
	}
	return 0;
}

static void probe_saadc(void)
{
	static nrf_saadc_value_t sample;

	if (!saadc_ready) {
		return;
	}
	if (nrfx_saadc_simple_mode_set(BIT(0), NRF_SAADC_RESOLUTION_12BIT,
				       NRF_SAADC_OVERSAMPLE_DISABLED, NULL) != 0) {
		return;
	}
	if (nrfx_saadc_buffer_set(&sample, 1) != 0) {
		return;
	}
	if (nrfx_saadc_mode_trigger() != 0) {
		return;
	}

	int code = (sample < 0) ? 0 : sample;
	int mv = (int)((int32_t)code * 3600 / 4096);
	float amps = (mv / 1000.0f) / (INA240_GAIN * INA240_SHUNT_OHMS);

	LOG_INF("AIN7: code=%d  %d mV  (~%d mA on fast channel)", code, mv,
		(int)(amps * 1000.0f));
}

void bringup_run(void)
{
	LOG_INF("=== pwr-logger bring-up probe (%s) ===", CONFIG_BOARD);

	if (gpio_is_ready_dt(&led)) {
		gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	} else {
		LOG_ERR("LED not ready");
	}
	LOG_INF("i2c0: %s", device_is_ready(i2c_bus) ? "ready" : "NOT ready");

	saadc_ready = (saadc_setup() == 0);
	LOG_INF("SAADC: %s", saadc_ready ? "ready (AIN7)" : "init FAILED");

	for (uint32_t n = 0;; n++) {
		gpio_pin_toggle_dt(&led);
		LOG_INF("--- probe #%u ---", n);
		probe_ina228();
		probe_sd();
		probe_saadc();
		k_sleep(K_SECONDS(2));
	}
}
