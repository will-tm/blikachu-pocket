#ifndef BLIKACHU_BL618_REBOOT_H
#define BLIKACHU_BL618_REBOOT_H

#include <zephyr/toolchain.h>

/* Reset into BootROM ISP without altering flash contents. */
FUNC_NORETURN void bl618_enter_isp(void);

#endif
