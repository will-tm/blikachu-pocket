#ifndef BLIKACHU_INPUT_H
#define BLIKACHU_INPUT_H

#include <stdbool.h>
#include <zephyr/input/input.h>

typedef void (*input_handler_t)(struct input_event *evt);

/* Set the active key handler. Pass NULL to clear. Replaces any previous
 * handler. The dispatcher itself runs from the input subsystem's thread
 * context — handlers must be quick / non-blocking.
 */
void input_set_handler(input_handler_t h);

/* Filter invoked for every event BEFORE the active handler. Return true
 * to consume the event (handler never sees it). Used by the system
 * hotkeys (SELECT+direction, START long-press) in main.c.
 */
typedef bool (*input_filter_t)(struct input_event *evt);
void input_set_filter(input_filter_t f);

/* Deliver a synthetic key event directly to the active handler
 * (bypasses the filter). Lets the hotkey filter re-inject a deferred
 * SELECT press/tap once it knows the press wasn't a hotkey chord.
 */
void input_inject(uint16_t code, bool pressed);

#endif
