/*
 * USB next-stack composite device: CDC ACM (console / shell) + DFU (BL618 firmware update).
 *
 * Replaces the legacy `usb_enable()` flow. Construct the descriptor
 * tree statically, register all linked classes, and bring the
 * controller up. The console UART node (`cdc_acm_uart0`) lives in the
 * board overlay under &zephyr_udc0 and is auto-discovered via the
 * shell of `usbd_register_all_classes`.
 */

#include <zephyr/device.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/logging/log.h>

#if defined(CONFIG_USBD_DFU)
#include "fw_install.h"
#endif
#include "usbd_setup.h"

LOG_MODULE_REGISTER(usbd_setup, LOG_LEVEL_INF);

USBD_DEVICE_DEFINE(g_blikachu_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), 0x2e8a, 0x000a);

USBD_DESC_LANG_DEFINE(g_lang);
USBD_DESC_MANUFACTURER_DEFINE(g_mfr, "BLikachu");
USBD_DESC_PRODUCT_DEFINE(g_product, "BLikachu Pocket");
USBD_DESC_CONFIG_DEFINE(g_cfg_desc, "FS Configuration");
USBD_DESC_CONFIG_DEFINE(g_hs_cfg_desc, "HS Configuration");

USBD_CONFIGURATION_DEFINE(g_fs_config, USB_SCD_SELF_POWERED, 200, &g_cfg_desc);
USBD_CONFIGURATION_DEFINE(g_hs_config, USB_SCD_SELF_POWERED, 200, &g_hs_cfg_desc);

/* USB DFU (src/bl618/usb_dfu.c): the DFU-mode instance sits in the
 * application configuration; the run-time DETACH instance is unused.
 */
static const char *const g_class_blocklist[] = {"dfu_runtime", NULL};

static usbd_msg_cb_t g_msg_cb;

void usbd_setup_register_msg_cb(usbd_msg_cb_t cb)
{
	g_msg_cb = cb;
}

static void msg_dispatch(struct usbd_context *ctx, const struct usbd_msg *msg)
{
	ARG_UNUSED(ctx);
#if defined(CONFIG_USBD_DFU)
	if (msg->type == USBD_MSG_RESET || msg->type == USBD_MSG_VBUS_REMOVED ||
	    (msg->type == USBD_MSG_CONFIGURATION && msg->status == 0)) {
		usb_dfu_cancel();
	}
#endif
	if (g_msg_cb) {
		g_msg_cb(NULL, msg);
	}
}

int usbd_setup(void)
{
	int err;

	err = usbd_add_descriptor(&g_blikachu_usbd, &g_lang);
	if (err) {
		LOG_ERR("add lang desc: %d", err);
		return err;
	}
	err = usbd_add_descriptor(&g_blikachu_usbd, &g_mfr);
	if (err) {
		LOG_ERR("add mfr desc: %d", err);
		return err;
	}
	err = usbd_add_descriptor(&g_blikachu_usbd, &g_product);
	if (err) {
		LOG_ERR("add product desc: %d", err);
		return err;
	}

	/* High-speed controller (BL618): register an HS configuration too —
	 * usbd_init() refuses an HS-capable UDC with only an FS config.
	 */
	if (usbd_caps_speed(&g_blikachu_usbd) == USBD_SPEED_HS) {
		err = usbd_add_configuration(&g_blikachu_usbd, USBD_SPEED_HS, &g_hs_config);
		if (err) {
			LOG_ERR("add hs configuration: %d", err);
			return err;
		}

		err = usbd_register_all_classes(&g_blikachu_usbd, USBD_SPEED_HS, 1, g_class_blocklist);
		if (err) {
			LOG_ERR("register hs classes: %d", err);
			return err;
		}

		usbd_device_set_code_triple(&g_blikachu_usbd, USBD_SPEED_HS,
					    USB_BCC_MISCELLANEOUS, 0x02, 0x01);
	}

	err = usbd_add_configuration(&g_blikachu_usbd, USBD_SPEED_FS, &g_fs_config);
	if (err) {
		LOG_ERR("add fs configuration: %d", err);
		return err;
	}

	err = usbd_register_all_classes(&g_blikachu_usbd, USBD_SPEED_FS, 1, g_class_blocklist);
	if (err) {
		LOG_ERR("register classes: %d", err);
		return err;
	}

	/* Composite device with CDC ACM (multi-interface) + DFU needs the
	 * IAD class triple (Miscellaneous, Common, IAD).
	 */
	usbd_device_set_code_triple(&g_blikachu_usbd, USBD_SPEED_FS, USB_BCC_MISCELLANEOUS, 0x02,
				    0x01);
	usbd_self_powered(&g_blikachu_usbd, true);

	(void)usbd_msg_register_cb(&g_blikachu_usbd, msg_dispatch);

	err = usbd_init(&g_blikachu_usbd);
	if (err) {
		LOG_ERR("usbd_init: %d", err);
		return err;
	}

	err = usbd_enable(&g_blikachu_usbd);
	if (err) {
		LOG_ERR("usbd_enable: %d", err);
		return err;
	}

	return 0;
}
