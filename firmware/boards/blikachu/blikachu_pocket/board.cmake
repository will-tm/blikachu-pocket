# SPDX-License-Identifier: Apache-2.0

# BL618 v2 hardware: flash over the BootROM UART protocol
# (bflb-mcu-tool auto-enters the ROM via RTS/DTR on the debug adapter).
# The BL618 shares the BL616 die id for the flasher.
board_runner_args(bflb_mcu_tool --chipname bl616)
include(${ZEPHYR_BASE}/boards/common/bflb_mcu_tool.board.cmake)
