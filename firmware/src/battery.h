#ifndef BLIKACHU_BATTERY_H
#define BLIKACHU_BATTERY_H

#include <stdint.h>

/* Sample VBAT once (blocking ADC read). Returns battery millivolts
 * (raw pin voltage x2 for the R26/R27 divider) or negative errno.
 * `raw_out` (optional) receives the raw 12-bit count for diagnostics.
 */
int battery_sample_mv(uint16_t *raw_out);

/* Rough LiPo open-circuit-voltage -> percent map. */
uint8_t battery_percent(uint16_t mv);

#endif
