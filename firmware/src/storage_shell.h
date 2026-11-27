#ifndef BLIKACHU_STORAGE_SHELL_H
#define BLIKACHU_STORAGE_SHELL_H

#include <stdbool.h>

/* USB mass-storage mode (the `usb` shell command / SELECT-hold toggle).
 * Implemented in storage_shell.c together with the mount/umount/mkfs/
 * disk/wipe/romaddr shell commands.
 */
void usb_mode_enter(void);
void usb_mode_exit(void);
bool usb_mode_active(void);

#endif
