/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BLIKACHU_SNES_READONLY_H
#define BLIKACHU_SNES_READONLY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Input is already normalized: no copier header, interleaving or padding. */
bool snes_readonly_validate(const void *data, size_t size, bool *hirom);
/* SNES address-line mirroring, including non-power-of-two ROMs. */
size_t snes_readonly_mirror(size_t size, size_t address);

#endif
