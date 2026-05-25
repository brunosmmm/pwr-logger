#include "ble_stream.h"
#include "logctl.h"
#include "clock.h"
#include "power.h"

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
#define PWR_OP_POWEROFF 0x06
#define PWR_OP_SETTIME  0x10   /* + u32 LE unix epoch */

/* Wire format of one telemetry notification (little-endian, packed). */
struct __packed pwr_telemetry {
	uint64_t t_start;   /* master tick of the window start */
	float    rms;       /* fast-channel RMS (raw code units for now) */
	float    avg;
	int16_t  min;
	int16_t  max;
	uint32_t n;
	uint16_t batt_mv;   /* VDDH (battery), cached */
	uint8_t  state;     /* enum logctl_state */
};

static volatile bool s_subscribed;
static int64_t s_last_notify;

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

static int start_adv(void)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), NULL, 0);

	if (err) {
		LOG_ERR("adv start -> %d", err);
	} else {
		LOG_INF("BLE advertising as '%s'", CONFIG_BT_DEVICE_NAME);
	}
	return err;
}

/* Want ~15-30 ms so the link can carry the ~20 Hz aggregate window stream
 * (BlueZ otherwise picks ~67 ms, which caps notifications at ~15 Hz). */
static const struct bt_le_conn_param fast_conn = BT_LE_CONN_PARAM_INIT(0x000c, 0x0018, 0, 400);

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
	LOG_INF("disconnected (0x%02x); re-advertising", reason);
	s_subscribed = false;
	k_work_submit(&adv_work);
}

BT_CONN_CB_DEFINE(conn_cbs) = {
	.connected = connected,
	.disconnected = disconnected,
	.le_param_updated = le_param_updated,
};

int ble_stream_init(void)
{
	int err = bt_enable(NULL);

	if (err) {
		LOG_ERR("bt_enable -> %d", err);
		return err;
	}
	return start_adv();
}

int ble_stream_publish(const struct agg_window *w)
{
	if (!s_subscribed) {
		return 0;
	}
	/* Flood guard: the aggregator emits 50 ms windows (~20 Hz), so cap a bit
	 * below that (25 Hz) to pass every window while dropping any burst. */
	int64_t now = k_uptime_get();

	if (now - s_last_notify < 40) {
		return 0;
	}
	s_last_notify = now;

	int batt = power_batt_mv();
	struct pwr_telemetry t = {
		.t_start = w->t_start,
		.rms = w->rms,
		.avg = w->avg,
		.min = w->min,
		.max = w->max,
		.n = w->n,
		.batt_mv = (batt > 0) ? (uint16_t)batt : 0,
		.state = (uint8_t)logctl_state(),
	};

	/* attrs[1] = characteristic value. Returns -ENOMEM when TX buffers are full. */
	return bt_gatt_notify(NULL, &pwr_svc.attrs[1], &t, sizeof(t));
}
