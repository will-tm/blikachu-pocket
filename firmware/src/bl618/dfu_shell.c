#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>
#include "dfu_request.h"
#include "emu.h"
#include "menu.h"
#include "storage.h"

static int cmd_dfu(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	emu_quit();
	emu_wait();
	storage_unmount();
	int rc = blikachu_dfu_request();
	if (rc) {
		shell_error(sh, "DFU request: %d", rc);
		menu_refresh();
		return rc;
	}
	shell_print(sh, "Rebooting to MCUboot DFU...");
	k_msleep(100);
	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}
SHELL_CMD_REGISTER(dfu, NULL, "Reboot into MCUboot USB DFU", cmd_dfu);
