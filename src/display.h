#ifndef DISPLAY_H_
#define DISPLAY_H_

/*
 * SSD1306 OLED status display (128x64, I2C on TWIM1). Built only when
 * CONFIG_APP_DISPLAY is set; otherwise display_init() is a no-op so the rest of
 * the app is unchanged when the panel isn't fitted.
 */

#if defined(CONFIG_APP_DISPLAY)
void display_init(void);
#else
static inline void display_init(void) { }
#endif

#endif /* DISPLAY_H_ */
