#ifndef BLIKACHU_PSRAM_BUF_H
#define BLIKACHU_PSRAM_BUF_H

/* Large app buffers live in the BL618's 4 MB PSRAM (cached, ~XIP-class
 * bandwidth per the bring-up bench) to keep the 480K SRAM for the
 * kernel, WiFi and hostap. NOLOAD region: never assume zero-init.
 */
#define BLIKACHU_BIGBUF __attribute__((section("PSRAM")))

#endif
