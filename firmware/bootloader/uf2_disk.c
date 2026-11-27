/* A virtual FAT16 medium; only validated UF2 data sectors become updates. */
#include <string.h>
#include <zephyr/drivers/disk.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/usb/class/usbd_msc.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/sys/byteorder.h>
#include <bootutil/bootutil_log.h>
#include "uf2_disk.h"
#include "usbd_dfu.h"
#include "bl618/reboot.h"

BOOT_LOG_MODULE_DECLARE(mcuboot);
#define SECTOR_SIZE 512u
#define DISK_SECTORS 32768u
#define FAT_SECTORS 128u
#define ROOT_SECTOR (1u + 2u * FAT_SECTORS)
#define DATA_SECTOR (ROOT_SECTOR + 2u)
#define UF2_DATA 256u
#define UF2_FAMILY 0xf3478d12u
#define IMAGE_BASE PARTITION_ADDRESS(slot0_partition)
#define STAGING_SIZE DT_REG_SIZE(DT_NODELABEL(core_ram))
#define BLOCK_MAX (STAGING_SIZE / UF2_DATA)
#define FLASH_SECTOR 4096u
#define SLOT1_ID PARTITION_ID(slot1_partition)
#define CACHE_SECTORS 512u
#define CACHE_MARK 0x8000u

/* Retain FAT/root mutations and small host metadata files. UF2 data is
 * reconstructed from the staged image, so no second copy is required. */
struct uf2_medium {
	uint8_t metadata[DATA_SECTOR][SECTOR_SIZE];
	uint16_t sector_map[DISK_SECTORS];
	uint16_t cache_lba[CACHE_SECTORS];
	uint8_t cache[CACHE_SECTORS][SECTOR_SIZE];
	unsigned cache_next;
};
BUILD_ASSERT(sizeof(struct uf2_medium) <= DT_REG_SIZE(DT_NODELABEL(psram)));
BUILD_ASSERT(BLOCK_MAX < CACHE_MARK);
static struct uf2_medium *const medium = (void *)DT_REG_ADDR(DT_NODELABEL(psram));

static uint8_t *const staged = (uint8_t *)DT_REG_ADDR(DT_NODELABEL(core_ram));
static uint8_t bitmap[DIV_ROUND_UP(BLOCK_MAX, 8)];
static unsigned blocks, received;
static unsigned progress;
static atomic_t writing;
static atomic_t rx_percent;
static atomic_t isp_pending;
static uint8_t verify[FLASH_SECTOR];
static const char info[] = "UF2 Bootloader BLikachu MCUboot\r\nModel: BLikachu Pocket\r\nBoard-ID: BLikachu-Pocket-BL618\r\n";

static void root_file(uint8_t *buf)
{
	memcpy(buf, "BLIKACHU   ", 11);
	buf[11] = 0x08;
	buf += 32;
	memcpy(buf, "INFO_UF2TXT", 11);
	buf[11] = 0x21;
	sys_put_le16(2, buf + 26);
	sys_put_le32(sizeof(info) - 1, buf + 28);
}

static void initial_sector(uint8_t *b, uint32_t sector)
{
	memset(b, 0, SECTOR_SIZE);
	if (sector == 0) {
		b[0] = 0xeb; b[1] = 0x3c; b[2] = 0x90;
		memcpy(b + 3, "MSDOS5.0", 8);
		sys_put_le16(SECTOR_SIZE, b + 11);
		b[13] = 1;
		sys_put_le16(1, b + 14);
		b[16] = 2;
		sys_put_le16(32, b + 17);
		sys_put_le16(DISK_SECTORS, b + 19);
		b[21] = 0xf8;
		sys_put_le16(FAT_SECTORS, b + 22);
		sys_put_le16(32, b + 24);
		sys_put_le16(64, b + 26);
		b[36] = 0x80; b[38] = 0x29;
		sys_put_le32(0x424c4b50, b + 39);
		memcpy(b + 43, "BLIKACHU   FAT16   ", 19);
		b[510] = 0x55; b[511] = 0xaa;
	} else if (sector == 1 || sector == 1 + FAT_SECTORS) {
		sys_put_le16(0xfff8, b);
		sys_put_le16(0xffff, b + 2);
		sys_put_le16(0xffff, b + 4);
	} else if (sector == ROOT_SECTOR) {
		root_file(b);
	} else if (sector == DATA_SECTOR) {
		memcpy(b, info, sizeof(info) - 1);
	}
}

static void cache_sector(uint32_t lba, const uint8_t *buf)
{
	uint16_t entry = medium->sector_map[lba];
	unsigned idx;
	if (entry & CACHE_MARK) {
		idx = (entry & ~CACHE_MARK) - 1;
	} else {
		idx = medium->cache_next++ % CACHE_SECTORS;
		uint16_t old = medium->cache_lba[idx];
		if (old && medium->sector_map[old] == (CACHE_MARK | (idx + 1))) {
			medium->sector_map[old] = 0;
		}
		medium->cache_lba[idx] = lba;
		medium->sector_map[lba] = CACHE_MARK | (idx + 1);
	}
	memcpy(medium->cache[idx], buf, SECTOR_SIZE);
}

static bool is_uf2(const uint8_t *b)
{
	return sys_get_le32(b) == 0x0a324655 && sys_get_le32(b + 4) == 0x9e5d5157 &&
	       sys_get_le32(b + 508) == 0x0ab16f30;
}

static bool is_isp_request(const uint8_t *b)
{
	static const char command[] = "BLIKACHU-ENTER-ISP";
	return is_uf2(b) && sys_get_le32(b + 8) == 0x2001 &&
	       sys_get_le32(b + 12) == 0 && sys_get_le32(b + 16) == UF2_DATA &&
	       sys_get_le32(b + 20) == 0 && sys_get_le32(b + 24) == 1 &&
	       sys_get_le32(b + 28) == UF2_FAMILY &&
	       !memcmp(b + 32, command, sizeof(command));
}

static void read_sector(uint8_t *b, uint32_t lba)
{
	if (lba < DATA_SECTOR) {
		memcpy(b, medium->metadata[lba], SECTOR_SIZE);
		return;
	}
	uint16_t entry = medium->sector_map[lba];
	if (entry & CACHE_MARK) {
		memcpy(b, medium->cache[(entry & ~CACHE_MARK) - 1], SECTOR_SIZE);
		return;
	}
	memset(b, 0, SECTOR_SIZE);
	if (entry && entry <= blocks) {
		unsigned block = entry - 1;
		sys_put_le32(0x0a324655, b);
		sys_put_le32(0x9e5d5157, b + 4);
		sys_put_le32(0x2000, b + 8);
		sys_put_le32(IMAGE_BASE + block * UF2_DATA, b + 12);
		sys_put_le32(UF2_DATA, b + 16);
		sys_put_le32(block, b + 20);
		sys_put_le32(blocks, b + 24);
		sys_put_le32(UF2_FAMILY, b + 28);
		memcpy(b + 32, staged + block * UF2_DATA, UF2_DATA);
		sys_put_le32(0x0ab16f30, b + 508);
	}
}

static int disk_read(struct disk_info *disk, uint8_t *buf, uint32_t start, uint32_t count)
{
	ARG_UNUSED(disk);
	if (start > DISK_SECTORS || count > DISK_SECTORS - start) {
		return -EINVAL;
	}
	while (count--) {
		read_sector(buf, start++);
		buf += SECTOR_SIZE;
	}
	return 0;
}

/* The MCUboot image begins with the MCUboot header and ends at its TLV table. */
static size_t image_len(size_t cap)
{
	if (cap < 0x200 || sys_get_le32(staged) != 0x96f3b83d || sys_get_le16(staged + 8) != 0x200) {
		return 0;
	}
	uint32_t body = sys_get_le32(staged + 12);
	uint16_t prot = sys_get_le16(staged + 10);
	if (body > cap - 0x200 || prot > cap - 0x200 - body) {
		return 0;
	}
	size_t end = 0x200 + body + prot;
	if (cap - end < 4 || sys_get_le16(staged + end) != 0x6907) {
		return 0;
	}
	uint16_t tlv = sys_get_le16(staged + end + 2);
	return tlv >= 4 && tlv <= cap - end ? end + tlv : 0;
}

static void reset_transfer(void)
{
	memset(bitmap, 0, sizeof(bitmap));
	for (unsigned i = DATA_SECTOR; i < DISK_SECTORS; i++) {
		if (!(medium->sector_map[i] & CACHE_MARK)) {
			medium->sector_map[i] = 0;
		}
	}
	blocks = received = 0;
	progress = 0;
	atomic_clear(&writing);
}

static void install(struct k_work *work)
{
	ARG_UNUSED(work);
	const struct flash_area *fa;
	size_t len = image_len(blocks * UF2_DATA);
	int rc = -EINVAL;
	if (!len || DIV_ROUND_UP(len, UF2_DATA) != blocks) {
		goto failed;
	}
	dfu_screen_message("WRITING FIRMWARE", "Keep power connected");
	dfu_screen_progress(0);
	rc = flash_area_open(SLOT1_ID, &fa);
	if (rc) {
		goto failed;
	}
	/* Swap-using-offset stores the candidate after the first erase sector.
	 * Invalidate any old trailer before programming the candidate. */
	rc = flash_area_erase(fa, fa->fa_size - 2 * FLASH_SECTOR, 2 * FLASH_SECTOR);
	if (!rc) {
		rc = flash_area_erase(fa, 0, FLASH_SECTOR);
	}
	for (size_t pos = 0; !rc && pos < len; pos += FLASH_SECTOR) {
		size_t chunk = MIN(FLASH_SECTOR, ROUND_UP(len - pos, 4));
		off_t off = FLASH_SECTOR + pos;
		rc = flash_area_erase(fa, off, FLASH_SECTOR);
		if (!rc) {
			rc = flash_area_write(fa, off, staged + pos, chunk);
		}
		if (!rc) {
			rc = flash_area_read(fa, off, verify, chunk);
			if (!rc && memcmp(verify, staged + pos, chunk)) {
				rc = -EIO;
			}
		}
		unsigned next = MIN(100u, (unsigned)((pos + chunk) * 100 / len));
		if (next / 10 != progress / 10) {
			dfu_screen_progress(next);
			progress = next;
		}
	}
	flash_area_close(fa);
	if (!rc) {
		blikachu_uf2_complete();
		return;
	}
failed:
	BOOT_LOG_ERR("UF2 install failed: %d", rc);
	reset_transfer();
	dfu_screen_message("UPDATE FAILED", "Copy UF2 again");
}
static K_WORK_DELAYABLE_DEFINE(install_work, install);

static void receive_progress(struct k_work *work)
{
	ARG_UNUSED(work);
	if (!atomic_get(&writing)) {
		dfu_screen_progress(atomic_get(&rx_percent));
	}
}
static K_WORK_DEFINE(receive_progress_work, receive_progress);

static void enter_isp_work(struct k_work *work)
{
	ARG_UNUSED(work);
	BOOT_LOG_INF("Entering BootROM ISP");
	dfu_screen_message("ISP MODE", "Connect flash tool");
	(void)usbd_disable(boot_usb_dfu_get_context());
	bl618_enter_isp();
}
static K_WORK_DELAYABLE_DEFINE(isp_work, enter_isp_work);

static int receive(const uint8_t *b)
{
	uint32_t block = sys_get_le32(b + 20), total = sys_get_le32(b + 24);
	if (sys_get_le32(b + 8) != 0x2000 || sys_get_le32(b + 28) != UF2_FAMILY ||
	    sys_get_le32(b + 16) != UF2_DATA || !total || total > BLOCK_MAX || block >= total ||
	    sys_get_le32(b + 12) != IMAGE_BASE + block * UF2_DATA) {
		BOOT_LOG_ERR("UF2 rejected: block %u/%u address 0x%08x", block, total,
			     sys_get_le32(b + 12));
		return -EINVAL;
	}
	if (atomic_get(&writing)) {
		return 0;
	}
	if (blocks && total != blocks) {
		reset_transfer();
	}
	if (!blocks) {
		BOOT_LOG_INF("UF2 receive: %u blocks", total);
		atomic_set(&rx_percent, 0);
		k_work_submit(&receive_progress_work);
	}
	blocks = total;
	uint8_t bit = BIT(block % 8);
	uint8_t *dst = staged + block * UF2_DATA;
	if (bitmap[block / 8] & bit) {
		if (memcmp(dst, b + 32, UF2_DATA)) {
			reset_transfer();
			return -EINVAL;
		}
		return 0;
	}
	memcpy(dst, b + 32, UF2_DATA);
	bitmap[block / 8] |= bit;
	received++;
	unsigned next = received * 100 / blocks;
	if (next / 10 != progress / 10) {
		BOOT_LOG_INF("UF2 receive: %u%%", next);
		atomic_set(&rx_percent, next);
		k_work_submit(&receive_progress_work);
		progress = next;
	}
	if (received == blocks) {
		atomic_set(&writing, 1);
		progress = 0;
		k_work_schedule(&install_work, K_MSEC(500));
	}
	return 0;
}

static int disk_write(struct disk_info *disk, const uint8_t *buf, uint32_t start, uint32_t count)
{
	ARG_UNUSED(disk);
	if (start > DISK_SECTORS || count > DISK_SECTORS - start) {
		return -EINVAL;
	}
	while (count--) {
		if (start < DATA_SECTOR) {
			memcpy(medium->metadata[start], buf, SECTOR_SIZE);
		} else if (is_isp_request(buf)) {
			if (!atomic_get(&isp_pending)) {
				if (!atomic_cas(&writing, 0, 1)) {
					return -EBUSY;
				}
				atomic_set(&isp_pending, 1);
				k_work_schedule(&isp_work, K_MSEC(500));
			}
			cache_sector(start, buf);
		} else if (is_uf2(buf)) {
			int rc = receive(buf);
			if (rc) {
				return rc;
			}
			medium->sector_map[start] = sys_get_le32(buf + 20) + 1;
		} else {
			cache_sector(start, buf);
		}
		start++;
		buf += SECTOR_SIZE;
	}
	return 0;
}

static int disk_ioctl(struct disk_info *disk, uint8_t cmd, void *buf)
{
	ARG_UNUSED(disk);
	switch (cmd) {
	case DISK_IOCTL_GET_SECTOR_COUNT: *(uint32_t *)buf = DISK_SECTORS; break;
	case DISK_IOCTL_GET_SECTOR_SIZE: *(uint32_t *)buf = SECTOR_SIZE; break;
	case DISK_IOCTL_GET_ERASE_BLOCK_SZ: *(uint32_t *)buf = 1; break;
	case DISK_IOCTL_CTRL_SYNC:
	case DISK_IOCTL_CTRL_INIT:
	case DISK_IOCTL_CTRL_DEINIT: break;
	default: return -EINVAL;
	}
	return 0;
}

static int disk_ok(struct disk_info *disk) { ARG_UNUSED(disk); return 0; }
static const struct disk_operations ops = {
	.init = disk_ok, .status = disk_ok, .read = disk_read, .write = disk_write, .ioctl = disk_ioctl,
};
static struct disk_info disk = { .name = "UF2", .ops = &ops };
USBD_DEFINE_MSC_LUN(uf2, "UF2", "BLikachu", "UF2 Recovery", "1.00");

int blikachu_uf2_init(void)
{
	memset(medium, 0, sizeof(*medium));
	for (unsigned i = 0; i < DATA_SECTOR; i++) {
		initial_sector(medium->metadata[i], i);
	}
	uint8_t sector[SECTOR_SIZE];
	initial_sector(sector, DATA_SECTOR);
	cache_sector(DATA_SECTOR, sector);
	return disk_access_register(&disk);
}
