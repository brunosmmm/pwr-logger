#include "controls.h"
#include "logctl.h"
#include "display.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(controls, LOG_LEVEL_INF);

/* All four pins live under /zephyr,user (see board overlay). */
#define ZU DT_PATH(zephyr_user)

static const struct gpio_dt_spec rec_g    = GPIO_DT_SPEC_GET(ZU, rec_gpios);
static const struct gpio_dt_spec rotate_g = GPIO_DT_SPEC_GET(ZU, rotate_gpios);
static const struct gpio_dt_spec offld_g  = GPIO_DT_SPEC_GET(ZU, offload_gpios);
static const struct gpio_dt_spec led_g    = GPIO_DT_SPEC_GET(ZU, led_gpios);

/* ---- timing constants ---------------------------------------------------- */
#define DEBOUNCE_MS         20      /* settle time after any edge */
#define REC_LONG_MS       2000      /* hold REC this long -> poweroff */
#define LED_TICK_MS         50      /* base period for the LED pattern engine */
#define ROTATE_NOTICE_MS  1000      /* display banner duration on rotate */

/* ROTATE ack pattern -- a leading 200 ms pause, three crisp 100 ms ON pulses
 * each separated by 100 ms OFF, and a 100 ms tail. ~850 ms total. Designed
 * to be unmistakable against every base state: starts with a long-enough
 * OFF gap to break the solid LOGGING baseline, and uses tight on/off cadence
 * (5 Hz) that doesn't match STOPPED (1 Hz) or OFFLOAD (2.5 Hz). */
static const uint8_t ack_pat[] = {
	0, 0, 0, 0,   /* 200 ms OFF (pause)  */
	1, 1,         /* 100 ms ON  (flash 1) */
	0, 0,         /* 100 ms OFF           */
	1, 1,         /* 100 ms ON  (flash 2) */
	0, 0,         /* 100 ms OFF           */
	1, 1,         /* 100 ms ON  (flash 3) */
	0, 0,         /* 100 ms OFF (tail)    */
};
#define ROTATE_FLASH_TICKS ((int)ARRAY_SIZE(ack_pat))

/* ---- LED pattern engine -------------------------------------------------- */
/* A 50 ms tick reads logctl_state() and an optional override (e.g. ROTATE
 * triple-flash) to decide the LED level. Counters are mod the longest
 * pattern period, so phase stays coherent across state changes. */

static atomic_t s_flash_ticks;   /* triple-flash remaining, drives override */

static void led_tick_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(led_tick_work, led_tick_fn);

static void led_tick_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	static uint32_t cnt;
	bool on;
	atomic_val_t flash = atomic_get(&s_flash_ticks);

	if (flash > 0 && flash <= ROTATE_FLASH_TICKS) {
		/* Play back ack_pat[] from start to end. `flash` counts down,
		 * so index = N - flash. */
		on = ack_pat[ROTATE_FLASH_TICKS - flash];
		atomic_dec(&s_flash_ticks);
	} else {
		switch (logctl_state()) {
		case LS_LOGGING: on = true;                       break;
		case LS_OFFLOAD: on = (cnt % 8u) < 4u;            break;  /* 200/200 ms */
		case LS_STOPPED: on = (cnt % 20u) < 10u;          break;  /* 500/500 ms */
		case LS_IDLE:    /* rail's off -- LED off too */
		case LS_SHIP:
		default:         on = false;                      break;
		}
	}

	gpio_pin_set_dt(&led_g, on);
	cnt++;
	k_work_schedule(&led_tick_work, K_MSEC(LED_TICK_MS));
}

/* ---- button debounce + edge dispatch ------------------------------------- */
/* On GPIO interrupt (any edge), schedule a per-button debounce work for
 * DEBOUNCE_MS; the work re-reads the line and dispatches the edge if the
 * level has actually changed since the last accepted edge. */

struct btn {
	const struct gpio_dt_spec *spec;
	struct gpio_callback cb;
	struct k_work_delayable debounce;
	int last_level;          /* logical level last accepted (0/1) */
	void (*on_edge)(int new_level);
};

static struct btn rec_btn;
static struct btn rotate_btn;
static struct btn offload_btn;

static void btn_isr(const struct device *d, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(d);
	ARG_UNUSED(pins);
	struct btn *b = CONTAINER_OF(cb, struct btn, cb);
	(void)k_work_reschedule(&b->debounce, K_MSEC(DEBOUNCE_MS));
}

static void btn_debounce_fn(struct k_work *w)
{
	struct btn *b = CONTAINER_OF(k_work_delayable_from_work(w), struct btn, debounce);
	int lvl = gpio_pin_get_dt(b->spec);
	if (lvl < 0) {
		return;
	}
	if (lvl == b->last_level) {
		/* glitch -- line already at the previously-accepted level */
		return;
	}
	b->last_level = lvl;
	b->on_edge(lvl);
}

static void btn_install(struct btn *b, const struct gpio_dt_spec *spec,
			void (*on_edge)(int))
{
	b->spec = spec;
	b->last_level = gpio_pin_get_dt(spec);
	b->on_edge = on_edge;
	k_work_init_delayable(&b->debounce, btn_debounce_fn);
	gpio_pin_configure_dt(spec, GPIO_INPUT);
	gpio_pin_interrupt_configure_dt(spec, GPIO_INT_EDGE_BOTH);
	gpio_init_callback(&b->cb, btn_isr, BIT(spec->pin));
	gpio_add_callback(spec->port, &b->cb);
}

/* ---- REC: short = LOG_TOGGLE, long = POWEROFF --------------------------- */
static atomic_t s_rec_long_fired;

static void rec_long_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	atomic_set(&s_rec_long_fired, 1);
	LOG_INF("REC long-press -> poweroff");
	logctl_post(EV_POWEROFF);
}
static K_WORK_DELAYABLE_DEFINE(rec_long_work, rec_long_fn);

static void rec_edge(int level)
{
	if (level == 1) {           /* pressed (active-low logical level) */
		atomic_set(&s_rec_long_fired, 0);
		k_work_schedule(&rec_long_work, K_MSEC(REC_LONG_MS));
	} else {                    /* released */
		k_work_cancel_delayable(&rec_long_work);
		if (atomic_get(&s_rec_long_fired) == 0) {
			LOG_INF("REC short-press -> toggle log");
			logctl_post(EV_REC_TOGGLE);
		}
	}
}

/* ---- ROTATE: short press only, also triggers LED triple-flash ----------- */
static void rotate_edge(int level)
{
	if (level != 1) {
		return;
	}
	enum logctl_state st = logctl_state();
	LOG_INF("ROTATE press (state=%s)", logctl_state_name(st));
	if (st == LS_LOGGING) {
		atomic_set(&s_flash_ticks, ROTATE_FLASH_TICKS);
		display_notice("rotated", ROTATE_NOTICE_MS);
		logctl_post(EV_ROTATE);
	} else if (st == LS_OFFLOAD) {
		display_notice("offload locked", 1500);
	} else {
		/* STOPPED / IDLE / SHIP -- rotate is meaningful only mid-log. */
		display_notice("logging only", 1500);
	}
}

/* ---- OFFLOAD: maintained -- level read, posts ON / OFF on each edge ----- */
static void offload_edge(int level)
{
	LOG_INF("OFFLOAD switch -> %s", level ? "ON" : "OFF");
	logctl_post(level ? EV_OFFLOAD_ON : EV_OFFLOAD_OFF);
}

/* ---- init --------------------------------------------------------------- */
void controls_init(void)
{
	if (!gpio_is_ready_dt(&rec_g) || !gpio_is_ready_dt(&rotate_g) ||
	    !gpio_is_ready_dt(&offld_g) || !gpio_is_ready_dt(&led_g)) {
		LOG_ERR("controls: GPIO ports not ready");
		return;
	}

	gpio_pin_configure_dt(&led_g, GPIO_OUTPUT_INACTIVE);

	btn_install(&rec_btn,     &rec_g,    rec_edge);
	btn_install(&rotate_btn,  &rotate_g, rotate_edge);
	btn_install(&offload_btn, &offld_g,  offload_edge);

	/* Seed OFFLOAD state from the switch's resting level. logctl is up
	 * (called after logctl_init()); the event drains into its msgq. */
	int sw = gpio_pin_get_dt(&offld_g);
	if (sw >= 0) {
		offload_btn.last_level = sw;
		LOG_INF("controls up; offload switch=%s at boot", sw ? "ON" : "OFF");
		logctl_post(sw ? EV_OFFLOAD_ON : EV_OFFLOAD_OFF);
	}

	k_work_schedule(&led_tick_work, K_MSEC(LED_TICK_MS));
}
