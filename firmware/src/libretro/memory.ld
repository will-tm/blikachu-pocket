/* SPDX-License-Identifier: Apache-2.0 */
/* Core code remains in XIP flash. Writable state shares a PSRAM region with
 * update staging and is initialized only after acquiring exclusive ownership.
 */
__lr_workspace_start = ORIGIN(CORE_RAM);
__lr_workspace_end = ORIGIN(CORE_RAM) + LENGTH(CORE_RAM);

.lr_common_data ORIGIN(CORE_RAM) :
{
	__lr_common_data_start = .;
	*liblr_common.a:*(.data .data.* .sdata .sdata.*)
	__lr_common_data_end = .;
} > CORE_RAM AT > ROM
__lr_common_data_load = LOADADDR(.lr_common_data);

.lr_common_bss (NOLOAD) :
{
	. = ALIGN(32);
	__lr_common_bss_start = .;
	*liblr_common.a:*(.bss .bss.* .sbss .sbss.* COMMON)
	. = ALIGN(32);
	__lr_common_bss_end = .;
} > CORE_RAM

OVERLAY __lr_common_bss_end :
{
	.lr_gambatte_data {
		__lr_gambatte_data_start = .;
		*liblr_gambatte.a:*(.data .data.* .sdata .sdata.*)
		__lr_gambatte_data_end = .;
	}
	.lr_fceumm_data {
		__lr_fceumm_data_start = .;
		*liblr_fceumm.a:*(.data .data.* .sdata .sdata.*)
		__lr_fceumm_data_end = .;
	}
	.lr_snes9x2005_data {
		__lr_snes9x2005_data_start = .;
		*liblr_snes9x2005.a:*(.data .data.* .sdata .sdata.*)
		__lr_snes9x2005_data_end = .;
	}
} > CORE_RAM AT > ROM
__lr_gambatte_data_load = LOADADDR(.lr_gambatte_data);
__lr_fceumm_data_load = LOADADDR(.lr_fceumm_data);
__lr_snes9x2005_data_load = LOADADDR(.lr_snes9x2005_data);

__lr_bss_start = ALIGN(__lr_common_bss_end +
	MAX(SIZEOF(.lr_gambatte_data),
	MAX(SIZEOF(.lr_fceumm_data), SIZEOF(.lr_snes9x2005_data))), 32);
OVERLAY __lr_bss_start :
{
	.lr_gambatte_bss {
		__lr_gambatte_bss_start = .;
		*liblr_gambatte.a:*(.bss .bss.* .sbss .sbss.* COMMON)
		. = ALIGN(32);
		__lr_gambatte_bss_end = .;
	}
	.lr_fceumm_bss {
		__lr_fceumm_bss_start = .;
		*liblr_fceumm.a:*(.bss .bss.* .sbss .sbss.* COMMON)
		. = ALIGN(32);
		__lr_fceumm_bss_end = .;
	}
	.lr_snes9x2005_bss {
		__lr_snes9x2005_bss_start = .;
		*liblr_snes9x2005.a:*(.bss .bss.* .sbss .sbss.* COMMON)
		. = ALIGN(32);
		__lr_snes9x2005_bss_end = .;
	}
} > CORE_RAM

ASSERT(LENGTH(CORE_RAM) == 3584 * 1024, "libretro workspace size mismatch")
ASSERT(__lr_gambatte_bss_end < __lr_workspace_end, "Gambatte leaves no heap")
ASSERT(__lr_fceumm_bss_end < __lr_workspace_end, "FCEUmm leaves no heap")
ASSERT(__lr_snes9x2005_bss_end < __lr_workspace_end, "Snes9x leaves no heap")
