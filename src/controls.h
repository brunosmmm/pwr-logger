#ifndef CONTROLS_H_
#define CONTROLS_H_

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

#endif /* CONTROLS_H_ */
