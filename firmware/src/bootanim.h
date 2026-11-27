#ifndef BLIKACHU_BOOTANIM_H
#define BLIKACHU_BOOTANIM_H

/* Play the boot animation once (blocking). Any button press skips the
 * remainder. Safe to call before the menu starts; no-op if the display
 * isn't ready.
 */
void boot_anim_play(void);

#endif
