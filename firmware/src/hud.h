#ifndef BLIKACHU_HUD_H
#define BLIKACHU_HUD_H

#include <stdbool.h>
#include <stdint.h>

/* Transient on-screen overlay ("VOL 7/11" + progress bar), shown for a
 * short time when volume/brightness change. Works in both contexts:
 * over the menu it draws directly and triggers a menu refresh on
 * expiry; while the emulator runs, the render thread composites it
 * over each outgoing block via hud_overlay().
 */

/* Show (or update) the overlay: "<label> <val>/<max>" + bar. */
void hud_show_level(const char *label, int val, int max);

/* Text-only variant (no bar): e.g. "STATE SAVED". */
void hud_show_text(const char *text);

/* Whether the active overlay intersects a block of panel rows. */
bool hud_overlaps(int panel_y, int rows);

/* Composite the active overlay into a block of panel rows about to be
 * pushed to the display. `panel_y` is the panel row of buf[0], `rows`
 * the block height, `stride` the row width in pixels. Returns false if
 * the HUD is inactive or doesn't intersect the block.
 */
bool hud_overlay(int panel_y, int rows, uint16_t *buf, int stride);

#endif
