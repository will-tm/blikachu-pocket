#include <zephyr/storage/flash_map.h>
#include "dfu_request.h"

#define DFU_MAGIC 0x55464442u
#define REQUEST_ID FIXED_PARTITION_ID(dfu_request_partition)

int blikachu_dfu_request(void)
{
	const struct flash_area *fa;
	uint32_t magic = DFU_MAGIC;
	int rc = flash_area_open(REQUEST_ID, &fa);

	if (!rc) {
		rc = flash_area_erase(fa, 0, fa->fa_size);
		if (!rc) {
			rc = flash_area_write(fa, 0, &magic, sizeof(magic));
		}
		flash_area_close(fa);
	}
	return rc;
}

bool blikachu_dfu_requested(void)
{
	const struct flash_area *fa;
	uint32_t magic;
	bool requested = false;

	if (flash_area_open(REQUEST_ID, &fa) == 0) {
		if (flash_area_read(fa, 0, &magic, sizeof(magic)) == 0 && magic == DFU_MAGIC) {
			requested = flash_area_erase(fa, 0, fa->fa_size) == 0;
		}
		flash_area_close(fa);
	}
	return requested;
}
