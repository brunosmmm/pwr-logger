#include "ble_stream.h"
#include "logctl.h"
#include "clock.h"
#include "power.h"
#include "acquisition.h"
#include "app_config.h"
#include "config.h"

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <time.h>

LOG_MODULE_REGISTER(ble_stream, LOG_LEVEL_INF);

/* Custom service: telemetry (notify) + control (write) characteristics. */
#define PWR_SVC_UUID  BT_UUID_128_ENCODE(0xf0a00000, 0x1234, 0x5678, 0x9abc, 0xdef012345678)
#define PWR_TELE_UUID BT_UUID_128_ENCODE(0xf0a00001, 0x1234, 0x5678, 0x9abc, 0xdef012345678)
#define PWR_CTRL_UUID BT_UUID_128_ENCODE(0xf0a00002, 0x1234, 0x5678, 0x9abc, 0xdef012345678)

static const struct bt_uuid_128 svc_uuid = BT_UUID_INIT_128(PWR_SVC_UUID);
static const struct bt_uuid_128 tele_uuid = BT_UUID_INIT_128(PWR_TELE_UUID);
static const struct bt_uuid_128 ctrl_uuid = BT_UUID_INIT_128(PWR_CTRL_UUID);

/* Control opcodes (1-byte op [+ payload]) written to the control characteristic. */
#define PWR_OP_LOG      0x01
#define PWR_OP_STOP     0x02
#define PWR_OP_ROTATE   0x03
#define PWR_OP_OFFLOAD  0x04
#define PWR_OP_RESUME   0x05
#define PWR_OP_POWEROFF  0x06
#define PWR_OP_KEEPALIVE 0x07   /* defer auto-sleep; ping periodically */
#define PWR_OP_SETTIME   0x10   /* + u32 LE unix epoch */

/* Tagged telemetry records (little-endian, packed). First byte is the type. */
#define TLM_FAST   0x01
#define TLM_SLOW   0x02
#define TLM_STATUS 0x03

struct __packed rec_fast {
	uint8_t  tag;
	uint32_t t_ms;
	float    avg;       /* fast-channel mean (raw SAADC code) */
	int16_t  min;
	int16_t  max;
	uint16_t n;
};

struct __packed rec_slow {
	uint8_t  tag;
	float    vbus_v;    /* INA228 bus (load) voltage */
	float    i_a;       /* INA228 current (precision) */
	float    p_w;
	float    temp_c;
};
/* Size = 17 bytes -- fits in the default-MTU notify payload cap of 20
 * (MTU 23 - 3 ATT overhead). Dropped t_ms: the host monitor only uses
 * the receive timestamp for its own UI, which is the same to within
 * the 5 Hz publish cadence, and rec_status carries no time at all.
 * The slow channel was being silently dropped at default MTU when this
 * record was 21 B; making it fit lets it work regardless of whether
 * the central negotiated MTU up. */

struct __packed rec_status {
	uint8_t  tag;
	uint8_t  state;     /* enum logctl_state */
	uint16_t batt_mv;   /* VDDH (battery) */
	uint32_t drops;     /* fast-channel cumulative drops */
};

/* INA228 register LSBs (match ina228.c). */
#define INA228_CURRENT_LSB (INA228_IMAX_A / 524288.0f)   /* A/LSB (2^19)  */
#define INA228_VBUS_V_LSB  195.3125e-6f
#define INA228_TEMP_C_LSB  7.8125e-3f

static volatile bool s_subscribed;

bool ble_stream_subscribed(void) { return s_subscribed; }
static int64_t s_last_fast;   /* FAST throttle */
static int64_t s_last_slow;   /* SLOW throttle */

static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	s_subscribed = (value == BT_GATT_CCC_NOTIFY);
	LOG_INF("telemetry notifications %s", s_subscribed ? "ON" : "OFF");
}

/* Control characteristic write: 1-byte opcode (+ payload) -> a logctl event or
 * a time sync. Mirrors the console `pwr` commands. */
static ssize_t ctrl_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	const uint8_t *p = buf;

	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len < 1) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	switch (p[0]) {
	case PWR_OP_LOG:      logctl_post(EV_LOG_START);   break;
	case PWR_OP_STOP:     logctl_post(EV_LOG_STOP);    break;
	case PWR_OP_ROTATE:   logctl_post(EV_ROTATE);      break;
	case PWR_OP_OFFLOAD:  logctl_post(EV_OFFLOAD_ON);  break;
	case PWR_OP_RESUME:   logctl_post(EV_OFFLOAD_OFF); break;
	case PWR_OP_POWEROFF: logctl_post(EV_POWEROFF);    break;
	case PWR_OP_KEEPALIVE: logctl_keepalive();          break;
	case PWR_OP_SETTIME:
		if (len >= 5) {
			uint32_t epoch;

			memcpy(&epoch, p + 1, sizeof(epoch));   /* LE */
			clk_set((time_t)epoch);
		}
		break;
	default:
		LOG_WRN("ble ctrl: unknown opcode 0x%02x", p[0]);
		break;
	}
	return len;
}

BT_GATT_SERVICE_DEFINE(pwr_svc,
	BT_GATT_PRIMARY_SERVICE(&svc_uuid),
	BT_GATT_CHARACTERISTIC(&tele_uuid.uuid, BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(&ctrl_uuid.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, ctrl_write, NULL),
);

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static bool s_adv;

/* Build a slow-adv parameter tuned for "device may be discoverable but
 * is not actively expected to be used right now" (LS_IDLE with
 * idle_ble=true, or LS_STOPPED on battery once the autooff grace window
 * elapses). Uses cfg.adv_idle_ms for the interval. Connectable so the
 * operator can still wake / interact -- just at a fraction of the FAST_1
 * radio duty cycle. */
static int start_adv_slow(void)
{
	uint32_t ms = config_get()->adv_idle_ms;

	if (ms < 100) {
		ms = 1000;        /* sanity default */
	}
	uint16_t units = (uint16_t)((ms * 8) / 5);   /* ms -> 0.625 ms units */
	struct bt_le_adv_param p = BT_LE_ADV_PARAM_INIT(
		BT_LE_ADV_OPT_CONN, units, units + 80, NULL);

	int err = bt_le_adv_start(&p, ad, ARRAY_SIZE(ad), NULL, 0);

	if (err) {
		LOG_ERR("adv (slow) start -> %d", err);
	} else {
		s_adv = true;
		LOG_INF("BLE advertising slow (~%u ms)", ms);
	}
	return err;
}

static int start_adv(void)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), NULL, 0);

	if (err) {
		LOG_ERR("adv start -> %d", err);
	} else {
		s_adv = true;
		LOG_INF("BLE advertising as '%s' (fast)", CONFIG_BT_DEVICE_NAME);
	}
	return err;
}

void ble_stream_set_adv(bool on)
{
	if (on && !s_adv) {
		/* Pick fast vs slow interval based on what the operator is
		 * doing right now: any state other than LS_IDLE means active
		 * use -> fast adv; LS_IDLE with idle_ble means "I want to be
		 * reachable but I'm on battery" -> slow adv. */
		if (logctl_state() == LS_IDLE) {
			(void)start_adv_slow();
		} else {
			(void)start_adv();
		}
	} else if (!on && s_adv) {
		(void)bt_le_adv_stop();
		s_adv = false;
		LOG_INF("BLE advertising stopped");
	}
}

/* Want ~15-30 ms so the link can carry the ~20 Hz aggregate window
 * stream (BlueZ otherwise picks ~67 ms, which caps notifications at
 * ~15 Hz). 4th arg is supervision timeout in 10 ms units: 1000 = 10 s,
 * matching CONFIG_BT_PERIPHERAL_PREF_TIMEOUT=1000 so long synchronous
 * flash ops (mcumgr image_erase ~5 s) don't drop the link. The earlier
 * value of 400 (4 s) overrode the bumped PPCP for the actual link. */
static const struct bt_le_conn_param fast_conn = BT_LE_CONN_PARAM_INIT(0x000c, 0x0018, 0, 1000);

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_WRN("connection failed (0x%02x)", err);
		return;
	}

	struct bt_conn_info info;

	if (bt_conn_get_info(conn, &info) == 0) {
		LOG_INF("connected: interval %u units (~%u ms)",
			info.le.interval, info.le.interval * 5 / 4);
	}
	/* Advertising auto-stopped on connect; ask the central to speed up. */
	(void)bt_conn_le_param_update(conn, &fast_conn);
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval,
			     uint16_t latency, uint16_t timeout)
{
	ARG_UNUSED(conn);
	LOG_INF("conn params updated: interval %u units (~%u ms) latency %u timeout %u ms",
		interval, interval * 5 / 4, latency, timeout * 10);
}

/* Restart advertising from the system workqueue, not the disconnected callback:
 * the connection object isn't fully released yet inside the callback, so a direct
 * bt_le_adv_start() there fails. */
static void adv_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	(void)start_adv();
}
static K_WORK_DEFINE(adv_work, adv_work_handler);

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	/* If we're in LS_IDLE and idle_ble is off, do NOT restart
	 * advertising. The earlier unconditional restart was the cause of
	 * "device says it's sleeping but BLE keeps responding" -- after
	 * any prior connection, FAST_1 adv stayed on in idle forever. */
	if (logctl_state() == LS_IDLE && !config_get()->idle_ble) {
		LOG_INF("disconnected (0x%02x); idle + idle_ble=off -> not re-advertising",
			reason);
		s_subscribed = false;
		return;
	}
	LOG_INF("disconnected (0x%02x); re-advertising", reason);
	s_subscribed = false;
	k_work_submit(&adv_work);
}

BT_CONN_CB_DEFINE(conn_cbs) = {
	.connected = connected,
	.disconnected = disconnected,
	.le_param_updated = le_param_updated,
};

/* Heartbeat: when subscribed but the aggregator isn't publishing (not logging),
 * still send state + battery every 500 ms so a central always sees the device
 * (n = 0 marks "no fast-channel data"). Suppressed while logging publishes flow. */
static void hb_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(hb_work, hb_fn);

void ble_stream_pause_hb(void)
{
	k_work_cancel_delayable(&hb_work);
}

void ble_stream_resume_hb(void)
{
	k_work_schedule(&hb_work, K_MSEC(500));
}

static void hb_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	if (s_subscribed) {
		int batt = power_batt_mv();
		struct rec_status r = {
			.tag = TLM_STATUS,
			.state = (uint8_t)logctl_state(),
			.batt_mv = (batt > 0) ? (uint16_t)batt : 0,
			.drops = acq_drops(),
		};

		(void)bt_gatt_notify(NULL, &pwr_svc.attrs[1], &r, sizeof(r));
	}
	k_work_schedule(&hb_work, K_MSEC(500));
}

int ble_stream_init(void)
{
	int err = bt_enable(NULL);

	if (err) {
		LOG_ERR("bt_enable -> %d", err);
		return err;
	}
	k_work_schedule(&hb_work, K_MSEC(500));
	return start_adv();
}

int ble_stream_publish(const struct agg_window *w)
{
	int64_t now = k_uptime_get();
	struct rec_fast r;

	if (!s_subscribed || (now - s_last_fast) < 40) {   /* ~20 Hz cap */
		return 0;
	}
	s_last_fast = now;

	r = (struct rec_fast){
		.tag = TLM_FAST,
		.t_ms = (uint32_t)now,
		.avg = w->avg,
		.min = w->min,
		.max = w->max,
		.n = (uint16_t)w->n,
	};
	/* attrs[1] = characteristic value. Returns -ENOMEM when TX buffers are full. */
	return bt_gatt_notify(NULL, &pwr_svc.attrs[1], &r, sizeof(r));
}

int ble_stream_publish_slow(const struct ina228_sample *s)
{
	int64_t now = k_uptime_get();
	struct rec_slow r;

	if (!s_subscribed || (now - s_last_slow) < 200) {   /* ~5 Hz cap */
		return 0;
	}
	s_last_slow = now;

	r = (struct rec_slow){
		.tag = TLM_SLOW,
		.vbus_v = (float)s->vbus_raw * INA228_VBUS_V_LSB,
		.i_a = (float)s->current_raw * INA228_CURRENT_LSB,
		.p_w = (float)s->power_raw * 3.2f * INA228_CURRENT_LSB,
		.temp_c = (float)s->temp_raw * INA228_TEMP_C_LSB,
	};
	return bt_gatt_notify(NULL, &pwr_svc.attrs[1], &r, sizeof(r));
}
