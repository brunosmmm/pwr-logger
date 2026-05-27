#ifndef DISPLAY_H_
#define DISPLAY_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * SSD1306 OLED status display (128x64, I2C on TWIM1). Built only when
 * CONFIG_APP_DISPLAY is set; otherwise display_init() is a no-op so the rest of
 * the app is unchanged when the panel isn't fitted.
 */

#if defined(CONFIG_APP_DISPLAY)
void display_init(void);
/* Power-cycle hook: call with false BEFORE the 3V3 rail is cut (so the render
 * thread stops writing to a chip that's about to lose power), and with true
 * AFTER the rail is restored (so the SSD1306 controller is fully re-initialized
 * -- it has no battery-backed config, so a rail cycle wipes its setup). */
void display_set_powered(bool on);
/* Overlay a short notice on the panel for the given duration. The render
 * thread shows `msg` plus a 1 s-resolution countdown in place of the normal
 * I/V/P layout. Pass duration_ms=0 to cancel any active notice. */
void display_notice(const char *msg, uint32_t duration_ms);
#else
static inline void display_init(void) { }
static inline void display_set_powered(bool on) { (void)on; }
static inline void display_notice(const char *msg, uint32_t duration_ms)
{ (void)msg; (void)duration_ms; }
#endif

#endif /* DISPLAY_H_ */
