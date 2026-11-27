/*
 * BL61x system reset.
 *
 * Zephyr's RISC-V sys_arch_reboot() is a weak no-op and the bl61x SoC
 * doesn't provide one, so sys_reboot() silently returns. This follows
 * the vendor GLB_SW_POR_Reset sequence: put the root clock back on
 * RC32M, then pulse the power-on reset. HBN_RSV0..3 are cleared first:
 * they sit in the always-on domain and survive software resets, and the
 * BootROM family uses them as a fast-boot handoff (BL70xL: see
 * soc/bflb/bl70xl/soc.c) — a stale value must not redirect the boot.
 *
 * RAM-resident and self-contained (inline register access only): the
 * DFU install calls it after rewriting the XIP flash.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/linker/sections.h>
#include <zephyr/sys/util.h>
#include <bouffalolab/bl61x/bflb_soc.h>
#include <bouffalolab/bl61x/glb_reg.h>
#include <bouffalolab/bl61x/hbn_reg.h>
#include <zephyr/drivers/clock_control/clock_control_bflb_common.h>

#if DT_PROP(DT_NODELABEL(sf_ctrl), use_32b_addresses)
/* RAM-resident hook from patches/0010; leaves XIP disabled. */
extern int flash_bflb_prepare_reset(void);
#endif

__ramfunc void sys_arch_reboot(int type)
{
	uint32_t v;

	ARG_UNUSED(type);

	(void)irq_lock();
#if DT_PROP(DT_NODELABEL(sf_ctrl), use_32b_addresses)
	/* The ROM starts in 3-byte mode. Do not fetch from XIP after this. */
	(void)flash_bflb_prepare_reset();
#endif
	sys_write32(0, HBN_BASE + HBN_RSV0_OFFSET);
	sys_write32(0, HBN_BASE + HBN_RSV1_OFFSET);
	sys_write32(0, HBN_BASE + HBN_RSV2_OFFSET);
	sys_write32(0, HBN_BASE + HBN_RSV3_OFFSET);
	clock_bflb_set_root_clock(BFLB_MAIN_CLOCK_RC32M);

	v = sys_read32(GLB_BASE + GLB_SWRST_CFG2_OFFSET);
	v &= ~(GLB_REG_CTRL_PWRON_RST_MSK | GLB_REG_CTRL_CPU_RESET_MSK |
	       GLB_REG_CTRL_SYS_RESET_MSK);
	sys_write32(v, GLB_BASE + GLB_SWRST_CFG2_OFFSET);
	v |= GLB_REG_CTRL_PWRON_RST_MSK;
	sys_write32(v, GLB_BASE + GLB_SWRST_CFG2_OFFSET);

	for (;;) {
	}
}
