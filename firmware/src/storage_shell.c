/* Storage and USB mass-storage mode for the handheld.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/sys/crc.h>
#include <zephyr/usb/class/usbd_msc.h>

#include "storage.h"
#include "storage_shell.h"
#include "usbd_setup.h"

/* Both handheld targets build the complete menu/input/render stack. */
#define BLIKACHU_FULL_UI 1
#include "emu.h"
#include "gfx.h"
#include "input.h"
#include "menu.h"

LOG_MODULE_REGISTER(storage_shell, LOG_LEVEL_INF);

/* ---- Filesystem / device shell commands ---- */

static int cmd_mount(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	int rc = storage_mount();
	if (rc) {
		shell_error(sh, "mount failed: %d", rc);
		return rc;
	}
	shell_print(sh, "mounted %s", STORAGE_MOUNT_POINT);
	return 0;
}
SHELL_CMD_REGISTER(mount, NULL, "Mount the NAND flash-disk internally", cmd_mount);

static int cmd_umount(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	int rc = storage_unmount();
	if (rc) {
		shell_error(sh, "unmount failed: %d", rc);
		return rc;
	}
	shell_print(sh, "unmounted %s", STORAGE_MOUNT_POINT);
	return 0;
}
SHELL_CMD_REGISTER(umount, NULL, "Release the NAND flash-disk to the USB host", cmd_umount);

static int cmd_mkfs(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	int rc = storage_mkfs();
	if (rc) {
		shell_error(sh, "mkfs failed: %d", rc);
		return rc;
	}
	shell_print(sh, "formatted %s as FAT", STORAGE_MOUNT_POINT);
	return 0;
}
SHELL_CMD_REGISTER(mkfs, NULL, "Format the NAND flash-disk as FAT", cmd_mkfs);

/* ---- USB mass-storage mode ---- */

#ifdef BLIKACHU_FULL_UI
#include "usb_icon.h" /* generated from assets/usb.png */

static void draw_usb_screen(void)
{
	gfx_clear(COLOR_BLACK);

	int x = (LCD_WIDTH - USB_ICON_W) / 2;
	int y = (LCD_HEIGHT - USB_ICON_H) / 2 - 40;
	(void)gfx_draw_image_rle(x, y, USB_ICON_W, USB_ICON_H,
				 usb_icon_rle, ARRAY_SIZE(usb_icon_rle));

	gfx_draw_text((LCD_WIDTH - 8 * 8) / 2, 120, "USB MODE", COLOR_PIKACHU, COLOR_BLACK);
	gfx_draw_text((LCD_WIDTH - 15 * 8) / 2, 144, "connect to host", COLOR_GRAY, COLOR_BLACK);
	gfx_draw_text((LCD_WIDTH - 13 * 8) / 2, 176, "press any key", COLOR_DGRAY, COLOR_BLACK);
	gfx_draw_text((LCD_WIDTH - 7 * 8) / 2, 192, "to exit", COLOR_DGRAY, COLOR_BLACK);
}
#endif /* BLIKACHU_FULL_UI */

static atomic_t g_usb_mode = ATOMIC_INIT(0);

bool usb_mode_active(void)
{
	return atomic_get(&g_usb_mode) != 0;
}

static void usb_exit_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	if (atomic_cas(&g_usb_mode, 1, 0)) {
		usbd_msc_set_medium(STORAGE_DISK_NAME, false);
#ifdef BLIKACHU_FULL_UI
		input_set_handler(NULL);
		menu_refresh();
#endif
	}
}
static K_WORK_DEFINE(usb_exit_work, usb_exit_work_fn);

void usbd_msc_on_eject(const char *disk)
{
	ARG_UNUSED(disk);
	k_work_submit(&usb_exit_work);
}

/* Device-initiated exit (button / shell / SELECT hold): flag the medium
 * out, then bounce the USB device — macOS polls an idle volume lazily
 * (if at all), so without the re-attach the stale mount lingers on the
 * host for minutes. Host-initiated ejects (usb_exit_work) skip the
 * bounce: the host already unmounted cleanly.
 * Blocks ~150 ms — call from a work item or the shell, NOT the input
 * dispatcher.
 */
void usb_mode_exit(void)
{
	if (!atomic_cas(&g_usb_mode, 1, 0)) {
		return;
	}
#ifdef BLIKACHU_FULL_UI
	emu_buzzer_blip(660, 60);
#endif
	usbd_msc_set_medium(STORAGE_DISK_NAME, false);
	(void)usbd_setup_reattach();
#ifdef BLIKACHU_FULL_UI
	input_set_handler(NULL);
	menu_refresh();
#endif
}

#ifdef BLIKACHU_FULL_UI
static void usb_force_exit_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	usb_mode_exit();
}
static K_WORK_DEFINE(usb_force_exit_work, usb_force_exit_work_fn);

static void usb_mode_input_cb(struct input_event *evt)
{
	if (evt->type == INPUT_EV_KEY && evt->value) {
		/* The re-attach blocks — hop off the input thread. */
		k_work_submit(&usb_force_exit_work);
	}
}
#endif /* BLIKACHU_FULL_UI */

void usb_mode_enter(void)
{
#ifdef BLIKACHU_FULL_UI
	emu_buzzer_blip(1760, 60);
#endif
	int rc = storage_unmount();

	if (rc != 0) {
		LOG_WRN("USB storage unavailable: %d", rc);
		return;
	}
#ifdef BLIKACHU_FULL_UI
	input_set_handler(NULL);

	gfx_init();
	draw_usb_screen();
#endif
	usbd_msc_set_medium(STORAGE_DISK_NAME, true);
	/* Re-attach so the host probes the fresh medium immediately
	 * instead of whenever it next feels like polling.
	 */
	(void)usbd_setup_reattach();
	atomic_set(&g_usb_mode, 1);
#ifdef BLIKACHU_FULL_UI
	input_set_handler(usb_mode_input_cb);
#endif
}

static int cmd_usb(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	/* Print + drain BEFORE the USB bounce kills the CDC session. */
	if (atomic_get(&g_usb_mode)) {
		shell_print(sh, "exiting USB mode");
		k_msleep(200);
		usb_mode_exit();
		return 0;
	}

	shell_print(sh, "entering USB mode — any button or `usb` again exits");
	k_msleep(200);
	usb_mode_enter();
	return 0;
}
SHELL_CMD_REGISTER(usb, NULL, "Toggle USB mass-storage mode", cmd_usb);

/* ---- Disk probes ---- */

static int cmd_disk(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	uint32_t bc = 0, bs = 0;
	int rc = disk_access_init(STORAGE_DISK_NAME);
	shell_print(sh, "disk_access_init(%s) = %d", STORAGE_DISK_NAME, rc);
	rc = disk_access_ioctl(STORAGE_DISK_NAME, DISK_IOCTL_GET_SECTOR_COUNT, &bc);
	shell_print(sh, "sector_count rc=%d -> %u", rc, bc);
	rc = disk_access_ioctl(STORAGE_DISK_NAME, DISK_IOCTL_GET_SECTOR_SIZE, &bs);
	shell_print(sh, "sector_size rc=%d -> %u (total %u MiB)", rc, bs,
		    (unsigned)((uint64_t)bc * bs / (1024 * 1024)));
	static uint8_t buf[512];
	rc = disk_access_read(STORAGE_DISK_NAME, buf, 0, 1);
	shell_print(sh, "read sector 0 rc=%d (%02x %02x %02x %02x)", rc, buf[0], buf[1], buf[2],
		    buf[3]);
	shell_print(sh, "sig=%02x%02x part_type=%02x", buf[0x1FE], buf[0x1FF], buf[0x1C2]);
	return 0;
}
SHELL_CMD_REGISTER(disk, NULL, "Probe NAND flash-disk status", cmd_disk);

static int cmd_wipe(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	int unmount_rc = storage_unmount();
	if (unmount_rc != 0) {
		return unmount_rc;
	}
	static uint8_t zero[512];
	memset(zero, 0, sizeof(zero));
	int rc = disk_access_init(STORAGE_DISK_NAME);
	if (rc) {
		shell_error(sh, "disk init failed: %d", rc);
		return rc;
	}
	rc = disk_access_write(STORAGE_DISK_NAME, zero, 0, 1);
	if (rc) {
		shell_error(sh, "write sector 0 failed: %d", rc);
		return rc;
	}
	rc = disk_access_ioctl(STORAGE_DISK_NAME, DISK_IOCTL_CTRL_SYNC, NULL);
	if (rc) {
		shell_error(sh, "sync failed: %d", rc);
		return rc;
	}
	shell_print(sh, "sector 0 wiped — macOS will see an unformatted disk");
	return 0;
}
SHELL_CMD_REGISTER(wipe, NULL, "Zero sector 0 so host can reformat", cmd_wipe);

static int cmd_romaddr(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "usage: romaddr <path>");
		return -EINVAL;
	}
	const char *err = NULL;
	size_t size;
	const uint8_t *p = resolve_file_xip(argv[1], &size, &err);
	if (!p) {
		shell_error(sh, "%s: %s", argv[1], err ? err : "unknown");
		return -EIO;
	}
	shell_print(sh, "%s -> %p (%zu bytes), header: %02x %02x %02x %02x", argv[1], p, size, p[0],
		    p[1], p[2], p[3]);
	return 0;
}
SHELL_CMD_REGISTER(romaddr, NULL, "Resolve /NAND: file to XIP pointer", cmd_romaddr);

static int cmd_romcrc(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	const char *err = NULL;
	size_t size;
	const uint8_t *rom = resolve_file_xip(argv[1], &size, &err);

	if (!rom) {
		shell_error(sh, "%s: %s", argv[1], err ? err : "unavailable");
		return -EIO;
	}
	uint32_t crc = 0;

	for (size_t offset = 0; offset < size;) {
		size_t bytes = MIN(size - offset, (size_t)65536);

		crc = crc32_ieee_update(crc, rom + offset, bytes);
		offset += bytes;
		k_sleep(K_MSEC(1));
	}
	shell_print(sh, "crc32=%08x bytes=%zu", crc, size);
	return 0;
}
SHELL_CMD_ARG_REGISTER(romcrc, NULL, "<path> — CRC32 of flash-mapped file bytes",
		      cmd_romcrc, 2, 0);
