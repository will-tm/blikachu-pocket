#ifndef BLIKACHU_PLATFORM_H
#define BLIKACHU_PLATFORM_H

/* BL618 hardware services implemented in platform_bl618.c. */

/* Early app init from main(): power latch engage, extra console
 * backends, anything the platform needs before the boot flow runs.
 */
void platform_init(void);

/* Warm reset back into the firmware (fault record in __noinit RAM
 * must survive).
 */
void platform_reboot(void);

/* Deepest power-off/sleep state, entered after the shared code has
 * already blanked the panel and unmounted storage. Does not return on
 * success (wake = full reboot). Returns a negative error when the
 * state was refused (e.g. externally powered) — the caller re-lights
 * the display.
 */
int platform_deep_sleep(void);

#endif
