/* SPDX-License-Identifier: Apache-2.0 */
#include "libretro/content.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	int failed = 0;
	static const char *const names[] = { "unknown", "GB", "GBC", "NES", "SNES" };

	for (int i = 1; i < argc; ++i) {
		FILE *f = fopen(argv[i], "rb");
		if (!f || fseek(f, 0, SEEK_END)) {
			perror(argv[i]);
			if (f) {
				fclose(f);
			}
			failed = 1;
			continue;
		}
		long size = ftell(f);
		if (size <= 0 || size > 8 * 1024 * 1024 + 512 || fseek(f, 0, SEEK_SET)) {
			fprintf(stderr, "%s: invalid test input size\n", argv[i]);
			fclose(f);
			failed = 1;
			continue;
		}
		uint8_t *data = malloc((size_t)size);
		if (!data || fread(data, 1, size, f) != (size_t)size) {
			fprintf(stderr, "%s: read failed\n", argv[i]);
			free(data);
			fclose(f);
			failed = 1;
			continue;
		}
		fclose(f);
		struct emu_content_info info;
		int rc = emu_content_identify(argv[i], data, size, &info);

		printf("%s: %s, %zu cartridge bytes, rc=%d\n", argv[i],
		       names[info.system], info.size, rc);
		failed |= rc != 0;
		free(data);
	}
	return failed;
}
