/*
 * Firmware update glue for the BL618 board. Two transports share one
 * image check + installer and the 3.5 MiB PSRAM
 * staging buffer (webui_staging_buf()):
 *   - USB DFU (src/bl618/usb_dfu.c): dfu-util with the build's update image
 *   - WiFi OTA: upload the same image to /NAND:/update.bin via the WebUI,
 *     then `ota apply` (shell) or the WebUI's OTA page.
 * MCUboot builds stage a signed image in the secondary slot. The BootROM
 * USB ISP (BOOT button) is the hardware recovery path.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include "emu.h"
#include "fw_install.h"
#include "storage.h"
#include "webui.h"

LOG_MODULE_REGISTER(ota, LOG_LEVEL_INF);

#if !defined(CONFIG_BOOTLOADER_MCUBOOT)
/* Raw UART0 marker (see resetinfo.c) — the only output that works
 * with IRQs locked.
 */
static __ramfunc void ota_mark(char c)
{
	*(volatile uint32_t *)(0x2000a000 + 0x88) = c;
	for (volatile int i = 0; i < 20000; i++) {
	}
}

#endif

uint8_t *dfu_staging_buf(size_t *cap)
{
	return webui_staging_claim(cap);
}

void dfu_staging_release(void)
{
	webui_staging_release();
}

void fw_install_prepare(void)
{
	extern void resetinfo_wdt_pause(void);

	/* Keep the feeder from re-arming the watchdog during installation. */
	resetinfo_wdt_pause();
	emu_pause();
	storage_unmount();
}

int ota_apply_file(const char *path)
{
	struct fs_file_t f;
	struct fs_dirent ent;
	size_t cap;
	size_t len;
	uint8_t *buf = webui_staging_claim(&cap);

	if (buf == NULL) {
		return -EBUSY;
	}
	if (fs_stat(path, &ent) != 0 || ent.size < 64 * 1024 || ent.size > cap) {
		LOG_ERR("%s: missing or implausible size", path);
		webui_staging_release();
		return -EINVAL;
	}

	fs_file_t_init(&f);
	if (fs_open(&f, path, FS_O_READ) != 0) {
		webui_staging_release();
		return -EIO;
	}
	ssize_t nread = fs_read(&f, buf, ent.size);

	fs_close(&f);
	if (nread != (ssize_t)ent.size) {
		LOG_ERR("read failed");
		webui_staging_release();
		return -EIO;
	}

	len = ent.size;
	if (fw_image_check(buf, &len) != 0) {
		webui_staging_release();
		return -EINVAL;
	}
	fw_install(buf, len);
	return 0; /* unreachable */
}

#if !defined(CONFIG_BOOTLOADER_MCUBOOT)
/* Minimal reproducer for the blast path: erase+write 4K at a given
 * offset with raw markers. `ota probe [hex-offset]`.
 */
static __ramfunc void ota_probe_blast(const struct device *fdev, const uint8_t *src,
				      uint32_t off)
{
	unsigned int key = irq_lock();
	int rc;

	ota_mark('E');
	rc = flash_erase(fdev, off, 4096);
	ota_mark(rc == 0 ? 'e' : '!');
	ota_mark('W');
	rc = flash_write(fdev, off, src, 4096);
	ota_mark(rc == 0 ? 'w' : '!');
	ota_mark('K');
	irq_unlock(key);
}

static int cmd_ota_probe(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *fdev = DEVICE_DT_GET(DT_CHOSEN(zephyr_flash_controller));
	uint32_t off = (argc > 1) ? strtoul(argv[1], NULL, 16) : 0;
	size_t cap;
	uint8_t *buf = webui_staging_claim(&cap);

	if (buf == NULL) {
		return -EBUSY;
	}

	/* pattern to write */
	for (int i = 0; i < 4096; i++) {
		buf[i] = (uint8_t)i;
	}
	shell_print(sh, "probe erase+write 4K @0x%x (markers on UART)", off);
	k_msleep(100);
	ota_probe_blast(fdev, buf, off);
	webui_staging_release();
	shell_print(sh, "probe done");
	return 0;
}

#endif

static int cmd_ota_apply(const struct shell *sh, size_t argc, char **argv)
{
	const char *path = (argc > 1) ? argv[1] : STORAGE_MOUNT_POINT "/update.bin";

	shell_print(sh, "applying %s...", path);
	return ota_apply_file(path);
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_ota,
	SHELL_CMD_ARG(apply, NULL, "[path] — flash update.bin and reboot", cmd_ota_apply, 1, 1),
#if !defined(CONFIG_BOOTLOADER_MCUBOOT)
	SHELL_CMD_ARG(probe, NULL, "[hex-off] — 4K erase+write reproducer", cmd_ota_probe, 1, 1),
#endif
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(ota, &sub_ota, "OTA firmware update", NULL);
