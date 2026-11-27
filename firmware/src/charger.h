#ifndef BLIKACHU_CHARGER_H
#define BLIKACHU_CHARGER_H

#include <stdbool.h>

/* True when the MCP73871 reports input power present (PG asserted) —
 * i.e. running from USB/charger rather than the battery.
 */
bool charger_input_present(void);

#endif
