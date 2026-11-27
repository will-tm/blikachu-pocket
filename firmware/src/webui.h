#ifndef BLIKACHU_WEBUI_H
#define BLIKACHU_WEBUI_H

#include <stddef.h>
#include <stdint.h>

/* Shared 3 MiB PSRAM staging buffer for HTTP uploads, OTA and USB DFU.
 * Claim before writing; release on failure or completion.
 */
uint8_t *webui_staging_buf(size_t *cap);
uint8_t *webui_staging_claim(size_t *cap);
void webui_staging_release(void);

#endif
