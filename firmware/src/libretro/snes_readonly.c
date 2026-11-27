/* SPDX-License-Identifier: Apache-2.0 */
#include "snes_readonly.h"

#include <string.h>

static bool header_valid(const uint8_t *data, size_t size, size_t offset,
                         bool hirom)
{
    if (offset > size || size - offset < 64)
        return false;
    const uint8_t *h = data + offset;
    unsigned mode = hirom ? 0x21 : 0x20;
    if ((h[0x15] & ~0x10) != mode || h[0x16] > 2 ||
        h[0x17] < 5 || h[0x17] > 12 || h[0x18] > 7 ||
        (!h[0x16] && h[0x18]) || h[0x19] > 13 || !(h[0x3d] & 0x80))
        return false;
    size_t declared = (size_t)1024 << h[0x17];
    unsigned checksum = h[0x1e] | (h[0x1f] << 8);
    unsigned complement = h[0x1c] | (h[0x1d] << 8);
    return size <= declared && size > declared / 2 && checksum &&
           (checksum + complement == 0xffff);
}

bool snes_readonly_validate(const void *data, size_t size, bool *hirom)
{
    /* These names select nonstandard maps or loader transformations upstream. */
    static const char *const unsupported[] = {
        "HONKAKUHA IGO GOSEI", "YUYU NO QUIZ DE GO!GO!",
        "SP MOMOTAROU DENTETSU2", "SUPER FORMATION SOCCE",
        "BATMAN--REVENGE JOKER", "THE DUEL: TEST DRIVE",
        "POPEYE IJIWARU MAJO", "POPN TWINBEE", "MEY Fun with Numbers",
        "WWF SUPER WRESTLEMANIA", "SOUND NOVEL-TCOOL", "DERBY STALLION 96",
        "THOROUGHBRED BREEDER3", "RPG-TCOOL 2", "DEZAEMON  ",
        "ADD-ON BASE CASSETE", "ROCKMAN X  ", "MEGAMAN X  ",
        "demon's blazon", "demon's crest", "HITOMI3",
        "WANDERERS FROM YS", "WAR 2410"
    };
    const uint8_t *rom = data;
    if (!rom || !hirom || size < 0x8000 || size > 0x400000 ||
        (size & 0x7fff) || !memcmp(rom, "BANDAI SFC-ADX", 14))
        return false;
    bool lo = header_valid(rom, size, 0x7fc0, false);
    bool hi = header_valid(rom, size, 0xffc0, true);
    if (lo == hi || (hi && (size & 0xffff)))
        return false;
    for (size_t offset = 0x7fc0; offset < size && offset <= 0xffc0;
         offset += 0x8000) {
        for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); i++) {
            if (!memcmp(rom + offset, unsupported[i], strlen(unsupported[i])))
                return false;
        }
    }
    *hirom = hi;
    return true;
}

size_t snes_readonly_mirror(size_t size, size_t address)
{
    size_t base = 0;
    size_t mask = 0x800000;
    while (address >= size) {
        while (!(address & mask))
            mask >>= 1;
        address -= mask;
        if (size > mask) {
            size -= mask;
            base += mask;
        }
        mask >>= 1;
    }
    return base + address;
}
