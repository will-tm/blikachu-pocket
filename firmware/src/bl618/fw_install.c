/*
 * BL61x firmware image check + in-place install (see fw_install.h).
 *
 * The install rewrites flash from offset 0 while every other piece of
 * code lives in that XIP region, so it runs from RAM with IRQs locked,
 * calls the flash driver through RAM copies of its device/API structs
 * and resets through the RAM-resident sys_arch_reboot() (reboot.c).
 * Power loss mid-install leaves a partial image; the BootROM USB ISP
 * (BOOT button) recovers it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>
#include <psa/crypto.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include "fw_install.h"

LOG_MODULE_REGISTER(fw_install, LOG_LEVEL_INF);

#define HDR_SIZE    0x2000
#define HDR_IMG_LEN 0x84
#define HDR_IMG_SHA 0x88
#define SECTOR      4096
#define XIP_BASE    0xa0000000u
#define UART0_WDATA 0x2000a088u

extern void sys_arch_reboot(int type);

int fw_image_check(const uint8_t *img, size_t *len)
{
	uint8_t sha[32];
	size_t sha_len;
	uint32_t app_len;
	psa_status_t st;

	if (*len <= HDR_SIZE || memcmp(img, "BFNP", 4) != 0) {
		LOG_ERR("not a whole-flash image (no BFNP header)");
		return -EINVAL;
	}
	/* BFNP header CRC covers its configuration and entry point as well
	 * as the embedded image hash. A payload hash alone cannot check it.
	 */
	if (crc32_ieee(img, 0xfc) != sys_get_le32(&img[0xfc])) {
		LOG_ERR("boot header CRC mismatch");
		return -EINVAL;
	}
	app_len = sys_get_le32(&img[HDR_IMG_LEN]);
	if (app_len == 0 || (size_t)app_len > *len - HDR_SIZE) {
		LOG_ERR("header app length %u exceeds %u received", app_len,
			(unsigned int)(*len - HDR_SIZE));
		return -EINVAL;
	}
	st = psa_crypto_init();
	if (st == PSA_SUCCESS) {
		st = psa_hash_compute(PSA_ALG_SHA_256, &img[HDR_SIZE], app_len, sha, sizeof(sha),
				      &sha_len);
	}
	if (st != PSA_SUCCESS) {
		LOG_ERR("sha256 failed: %d", st);
		return -EINVAL;
	}
	if (memcmp(sha, &img[HDR_IMG_SHA], sizeof(sha)) != 0) {
		LOG_ERR("app SHA-256 does not match the boot header");
		return -EINVAL;
	}
	*len = HDR_SIZE + app_len;
	return 0;
}

__weak void fw_install_prepare(void)
{
}

/* Raw UART0 marker — the only output that works with IRQs locked. */
static __ramfunc void mark(char c)
{
	*(volatile uint32_t *)UART0_WDATA = c;
	for (volatile int i = 0; i < 20000; i++) {
	}
}

/* RAM copies of the dispatch state: the install erases the flash
 * sectors holding the device/API structs mid-run.
 */
static struct device g_dev;
static struct flash_driver_api g_api;

static __ramfunc FUNC_NORETURN void blast(const uint8_t *img, size_t len)
{
	const volatile uint8_t *xip = (const volatile uint8_t *)XIP_BASE;
	flash_api_write fw = g_api.write;
	flash_api_erase fe = g_api.erase;

	(void)irq_lock();
	/* Per-sector erase+write: a single multi-MB erase leaves the
	 * controller hanging on the next write.
	 */
	mark('W');
	for (size_t off = 0; off < len; off += SECTOR) {
		size_t chunk = MIN((size_t)SECTOR, len - off);

		if (fe(&g_dev, off, SECTOR) != 0 || fw(&g_dev, off, &img[off], chunk) != 0) {
			mark('!');
			goto reset;
		}
		if ((off & (128 * 1024 - 1)) == 0) {
			mark('.');
		}
	}
	/* XIP is back after the last op and maps flash 1:1, but the
	 * D-cache still holds lines of the old image: write back + drop
	 * everything (the staged image's dirty PSRAM lines included)
	 * before reading the new flash. A mismatch then means silent
	 * corruption: spin instead of booting garbage.
	 */
	__asm__ volatile("fence\n"
			 "th.dcache.ciall\n"
			 "fence\n" ::: "memory");
	for (size_t i = 0; i < len; i++) {
		if (xip[i] != img[i]) {
			mark('!');
			mark('V');
			for (;;) {
			}
		}
	}
	mark('O');
	mark('K');
reset:
	sys_arch_reboot(0);
	for (;;) {
	}
}

FUNC_NORETURN void fw_install(const uint8_t *img, size_t len)
{
	const struct device *fdev = DEVICE_DT_GET(DT_CHOSEN(zephyr_flash_controller));

	LOG_WRN("installing %u bytes, then reset (UART0 shows W...OK)", (unsigned int)len);
	fw_install_prepare();
#if defined(CONFIG_WATCHDOG) && DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(wdt0))
	/* The install runs for seconds with IRQs locked: no feeding. */
	(void)wdt_disable(DEVICE_DT_GET(DT_NODELABEL(wdt0)));
#endif
	k_msleep(50);

	memcpy(&g_dev, fdev, sizeof(g_dev));
	memcpy(&g_api, fdev->api, sizeof(g_api));
	g_dev.api = &g_api;
	blast(img, len);
}
