/*
 * USB DFU firmware update (dfu-util) for the BL618 boards.
 *
 * Payload: build/zephyr/zephyr.dfu.bin (whole-flash image, see
 * fw_install.h). DNLOAD blocks are staged in RAM (dfu_staging_buf(),
 * provided by the app) — flash is untouched until the final zero-length
 * DNLOAD has passed fw_image_check(); a bad image fails the transfer.
 * After the host's last status poll (dfuMANIFEST-WAIT-RESET) the image
 * is installed and the device resets.
 *
 * The DFU-mode instance ("dfu_dfu") sits in the application USB
 * configuration; register classes with "dfu_runtime" blocklisted.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_dfu.h>

#include "fw_install.h"

LOG_MODULE_REGISTER(usb_dfu, LOG_LEVEL_INF);

static uint8_t *g_img;
static size_t g_cap;
static size_t g_len;
static size_t g_block_size;
static bool g_valid;
static struct k_work_delayable g_install_work;
static bool g_install_pending;
static K_MUTEX_DEFINE(g_lock);
static int64_t g_last_block;
static struct k_work_delayable g_timeout_work;

static void staging_release(void)
{
	if (g_img != NULL) {
		dfu_staging_release();
		g_img = NULL;
	}
	g_valid = false;
}

static void install_work(struct k_work *work)
{
	ARG_UNUSED(work);
	fw_install(g_img, g_len);
}

static int dfu_write_locked(void *const priv, const uint32_t block, const uint16_t size,
		     const uint8_t buf[static CONFIG_USBD_DFU_TRANSFER_SIZE])
{
	ARG_UNUSED(priv);

	if (g_install_pending) {
		return -EBUSY;
	}
	if (block == 0) {
		staging_release();
		g_img = dfu_staging_buf(&g_cap);
		g_len = 0;
		g_block_size = size;
		g_valid = false;
	}
	if (g_img == NULL) {
		LOG_ERR("no staging buffer");
		return -EBUSY;
	}
	if (size == 0) {
		g_valid = (fw_image_check(g_img, &g_len) == 0);
		if (!g_valid) {
			staging_release();
			return -EINVAL;
		}
		LOG_INF("image OK: %u bytes, SHA-256 matches", (unsigned int)g_len);
		return 0;
	}
	/* The host picks the block size (dfu-util -t); all but the last
	 * block carry exactly that many bytes.
	 */
	if ((size_t)block * g_block_size != g_len) {
		LOG_ERR("block %u out of sequence", block);
		staging_release();
		return -EINVAL;
	}
	if (g_len + size > g_cap) {
		LOG_ERR("image larger than %u bytes", (unsigned int)g_cap);
		staging_release();
		return -EFBIG;
	}
	g_last_block = k_uptime_get();
	k_work_reschedule(&g_timeout_work, K_SECONDS(30));
	memcpy(&g_img[g_len], buf, size);
	g_len += size;
	return 0;
}

static bool dfu_next_locked(void *const priv, const enum usb_dfu_state state,
		     const enum usb_dfu_state next)
{
	ARG_UNUSED(priv);

	/* MANIFEST_SYNC → IDLE would skip manifestation: refuse it so
	 * the class goes MANIFEST → MANIFEST_WAIT_RST.
	 */
	if (state == DFU_MANIFEST_SYNC && next == DFU_IDLE) {
		return !g_valid;
	}
	if (next == DFU_MANIFEST_WAIT_RST && g_valid) {
		/* Let the status reply reach the host first. */
		g_install_pending = true;
		k_work_schedule(&g_install_work, K_MSEC(200));
	}
	if ((next == DFU_IDLE || next == DFU_ERROR) && !g_install_pending) {
		staging_release();
	}
	return true;
}

/* Message callbacks, control requests and the timeout run on different
 * threads. Serialize them so cancellation cannot release a buffer while a
 * download callback is copying into it.
 */
void usb_dfu_cancel(void)
{
	k_mutex_lock(&g_lock, K_FOREVER);
	if (!g_install_pending) {
		staging_release();
	}
	k_mutex_unlock(&g_lock);
}

static void timeout_work(struct k_work *work)
{
	ARG_UNUSED(work);
	k_mutex_lock(&g_lock, K_FOREVER);
	if (!g_install_pending && g_img != NULL && k_uptime_get() - g_last_block >= 30000) {
		staging_release();
	}
	k_mutex_unlock(&g_lock);
}

static int dfu_write(void *const priv, const uint32_t block, const uint16_t size,
		     const uint8_t buf[static CONFIG_USBD_DFU_TRANSFER_SIZE])
{
	k_mutex_lock(&g_lock, K_FOREVER);
	int rc = dfu_write_locked(priv, block, size, buf);

	k_mutex_unlock(&g_lock);
	return rc;
}

static bool dfu_next(void *const priv, const enum usb_dfu_state state,
		     const enum usb_dfu_state next)
{
	k_mutex_lock(&g_lock, K_FOREVER);
	bool ready = dfu_next_locked(priv, state, next);

	k_mutex_unlock(&g_lock);
	return ready;
}

USBD_DFU_DEFINE_IMG(flash, "flash", NULL, NULL, dfu_write, dfu_next);

static int usb_dfu_init(void)
{
	k_work_init_delayable(&g_install_work, install_work);
	k_work_init_delayable(&g_timeout_work, timeout_work);
	return 0;
}
SYS_INIT(usb_dfu_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
