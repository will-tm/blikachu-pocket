/* Signed updates are staged to the secondary slot; MCUboot validates and swaps. */
#include <errno.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/storage/flash_map.h>
#include "fw_install.h"

LOG_MODULE_REGISTER(fw_install, LOG_LEVEL_INF);

#define MCU_IMAGE_MAGIC 0x96f3b83dU
#define MCU_TLV_MAGIC 0x6907U
#define MCU_PROT_TLV_MAGIC 0x6908U

int fw_image_check(const uint8_t *img, size_t *len)
{
	if (*len < 32 || sys_get_le32(img) != MCU_IMAGE_MAGIC ||
	    *len > DT_REG_SIZE(DT_NODELABEL(slot0_partition)) - CONFIG_MCUBOOT_UPDATE_FOOTER_SIZE) {
		return -EINVAL;
	}
	uint16_t hdr = sys_get_le16(img + 8);
	uint16_t prot = sys_get_le16(img + 10);
	uint32_t payload = sys_get_le32(img + 12);
	if (hdr != CONFIG_ROM_START_OFFSET || hdr > *len || payload > *len - hdr) {
		return -EINVAL;
	}
	size_t pos = hdr + payload;
	if (prot) {
		if (prot < 4 || prot > *len - pos || sys_get_le16(img + pos) != MCU_PROT_TLV_MAGIC ||
		    sys_get_le16(img + pos + 2) != prot) {
			return -EINVAL;
		}
		pos += prot;
	}
	if (*len - pos < 4 || sys_get_le16(img + pos) != MCU_TLV_MAGIC) {
		return -EINVAL;
	}
	uint16_t tlv = sys_get_le16(img + pos + 2);
	if (tlv < 4 || tlv > *len - pos) {
		return -EINVAL;
	}
	*len = pos + tlv;
	return 0;
}

FUNC_NORETURN void fw_install(const uint8_t *img, size_t len)
{
	static struct flash_img_context ctx;
	fw_install_prepare();
	int rc = flash_img_init(&ctx);
	if (!rc) {
		rc = flash_img_buffered_write(&ctx, img, len, true);
	}
	if (!rc) {
		rc = boot_request_upgrade(BOOT_UPGRADE_TEST);
	}
	if (rc) {
		LOG_ERR("secondary slot update failed: %d", rc);
	}
	sys_reboot(SYS_REBOOT_COLD);
	CODE_UNREACHABLE;
}
