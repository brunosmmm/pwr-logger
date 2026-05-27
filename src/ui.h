#ifndef UI_H_
#define UI_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * Multi-page UI. The display thread calls ui_render(disp) every render cycle;
 * everything else (page state, format helpers, status-bar composition) lives
 * here. Pages cycle on button events posted through ui_next_page() /
 * ui_set_page() (Phase 3) or via the CLI / BLE (Phase 4 wiring).
 *
 * Threading: ui_render runs only in the display thread (single reader of
 * cfb_* state). The page-state setters (ui_next_page / ui_set_page /
 * ui_kick / ui_hold_clear_current) can be called from any context; they
 * touch only atomic / mutex-protected state.
 */

struct device;   /* fwd-decl: avoid pulling zephyr/device.h into all callers */

enum ui_page {
	PAGE_LIVE = 0,
	PAGE_ROLL,
	PAGE_PEAKS,
	PAGE_HOLD,
	PAGE_ENERGY,
	PAGE_FILE,
	PAGE_SYS,
	PAGE__COUNT,
};

void ui_init(void);
void ui_render(const struct device *disp);   /* called from display.c render loop */

void ui_next_page(void);                      /* ROTATE short */
void ui_prev_page(void);
void ui_set_page(enum ui_page p);             /* CLI / BLE / boot default */
void ui_kick(void);                           /* any button: bump last-input ms */
void ui_hold_clear_current(void);             /* ROTATE long on a HOLD-related page */
enum ui_page ui_get_page(void);

/* Milliseconds since the last ui_kick() (i.e. last button press). Used by
 * display.c to decide when to blank the panel for idle. */
int64_t ui_idle_ms(void);

#endif /* UI_H_ */
