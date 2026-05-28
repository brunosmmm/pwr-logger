#ifndef CONTROLS_H_
#define CONTROLS_H_

#include <stdbool.h>

/*
 * Front-panel controls: REC / ROTATE momentary buttons, OFFLOAD maintained
 * switch, plus a status LED. All inputs are GPIO interrupts with software
 * debounce (~20 ms); REC distinguishes short press (toggle log) from long
 * press (poweroff). The LED runs a 50 ms tick that picks a pattern from
 * logctl_state() (solid=LOGGING, slow=STOPPED, fast=OFFLOAD, off=IDLE/SHIP)
 * with a triple-flash overlay on ROTATE acknowledge.
 *
 * Call AFTER logctl_init() -- the boot-time seeding for OFFLOAD switch
 * posts events into the logctl msgq.
 */
void controls_init(void);

/* DIAG: pause the led_tick engine and force the LED GPIO to a state for
 * hardware probing. mode: 1 = drive HIGH, 0 = drive LOW, -1 = INPUT NOPULL
 * (tristate -- any glow with this set is external to the chip),
 * anything else = resume the led_tick engine. Combined with a DMM on the
 * pin this separates "GPIO can't sink" from "external current source". */
void controls_led_diag(int mode);

#endif /* CONTROLS_H_ */
