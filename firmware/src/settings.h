#ifndef BLIKACHU_SETTINGS_H
#define BLIKACHU_SETTINGS_H

#include <stddef.h>

/* Tiny persisted key/value store: `key=value` lines in
 * /NAND:/.blikachu.cfg (host-editable over USB MSC). Whole-file
 * read/rewrite under a mutex — fine for a handful of small settings,
 * not a database. Requires /NAND: to be mounted.
 */

/* Copy the value for `key` into `out` (NUL-terminated). Returns 0,
 * -ENOENT if the key (or file) is missing, -EMSGSIZE if `cap` is too
 * small.
 */
int settings_get_str(const char *key, char *out, size_t cap);

/* Set/replace `key`. Returns 0 or negative errno (-ENOMEM if the file
 * would exceed the settings buffer).
 */
int settings_set_str(const char *key, const char *val);

/* Integer conveniences on top of the string API. */
int settings_get_int(const char *key, int fallback);
int settings_set_int(const char *key, int val);

/* Remove `key`. Returns 0 (also when the key didn't exist) or negative
 * errno on I/O failure.
 */
int settings_delete(const char *key);

/* Print all entries through `out` (one "key=value" per call) — for the
 * `cfg` shell command.
 */
void settings_list(void (*out)(void *ctx, const char *line), void *ctx);

#endif
