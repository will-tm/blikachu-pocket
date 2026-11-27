/*
 * BL61x firmware image check and install (shared by USB DFU and WiFi OTA).
 *
 * Image format: the whole-flash image the BootROM boots from offset 0
 * (build/zephyr/zephyr.dfu.bin, tools/bflb_whole_img.py): an 8 KiB BFNP
 * boot header holding the app length (+0x84) and its SHA-256 (+0x88),
 * followed by the app. MCUboot builds accept a signed application image
 * and stage it in the secondary slot instead.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BLIKACHU_FW_INSTALL_H
#define BLIKACHU_FW_INSTALL_H

#include <stddef.h>
#include <stdint.h>
#include <zephyr/toolchain.h>

/**
 * Validate a staged image's format and bounds. The legacy format also
 * checks the BFNP header CRC and SHA-256; MCUboot authenticates the
 * signed candidate before swapping slots.
 *
 * @param img Staged image.
 * @param len In: bytes staged. Out: header + app length (trailing
 *            padding dropped).
 * @retval 0 Image valid.
 * @retval -EINVAL Not a valid image.
 */
int fw_image_check(const uint8_t *img, size_t *len);

/**
 * Install a checked image and reset. Legacy firmware rewrites from
 * offset zero in RAM with IRQs locked; MCUboot firmware writes only the
 * secondary slot and requests a test upgrade. Never returns.
 */
FUNC_NORETURN void fw_install(const uint8_t *img, size_t len);

/** Hook for the app to quiesce (unmount storage, ...); default empty. */
void fw_install_prepare(void);

/**
 * RAM buffer the USB DFU download is staged in (usb_dfu.c), provided by
 * the app. Must hold a whole image (8 KiB header + app).
 *
 * @param cap Out: buffer size in bytes.
 * @return Buffer, or NULL if it is unavailable right now.
 */
uint8_t *dfu_staging_buf(size_t *cap);

/** Release a claimed DFU staging buffer after abort or validation failure. */
void dfu_staging_release(void);

/** Cancel an incomplete DFU transfer on USB reset/disconnect. */
void usb_dfu_cancel(void);

#endif
