#ifndef BLIKACHU_MENU_H
#define BLIKACHU_MENU_H

#include <stddef.h>

#define ROM_NAME_MAX 56

/* Boot the menu thread and its supporting state. Idempotent. */
void menu_start(void);

/* Re-scan /NAND: for ROMs and redraw. Call after the host has finished
 * writing files (e.g. on USB MSC eject).
 */
void menu_refresh(void);

/* Programmatically launch a ROM by absolute path; used by `run` shell
 * cmd. Blocks the caller until the emulator exits.
 */
int menu_launch(const char *path);

/* Print the ROM list to a callback (used by the shell). */
typedef void (*menu_print_fn)(void *ctx, const char *line);
void menu_list(menu_print_fn cb, void *ctx);

/* Look up the Nth ROM (0-based) by the order returned in menu_list. The
 * filename (no path) is copied into `out`. Triggers a re-scan first.
 * Returns 0 on success, -ERANGE if `idx` is out of bounds, -ENOENT if
 * no ROMs are present.
 */
int menu_get_rom(int idx, char *out, size_t cap);

#endif
