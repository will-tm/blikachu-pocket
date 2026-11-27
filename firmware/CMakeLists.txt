#
# CMakeLists.txt for blikachu-pocket
#
# SPDX-License-Identifier: Apache-2.0
#

cmake_minimum_required(VERSION 3.20.0)

# Custom board blikachu_pocket lives in boards/blikachu/.
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project("blikachu-pocket")

include(cmake/libretro.cmake)
zephyr_linker_sources(ROM_START ${CMAKE_CURRENT_SOURCE_DIR}/src/libretro/initializers.ld)
zephyr_linker_sources(RAM_SECTIONS ${CMAKE_CURRENT_SOURCE_DIR}/src/libretro/memory.ld)
option(BLIKACHU_LIBRETRO_PROBE "Link headless libretro hardware diagnostics" ON)
if(BLIKACHU_LIBRETRO_PROBE)
    target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/libretro/probe.cpp)
endif()
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/deps/libretro/common/include)
target_link_libraries(app PRIVATE lr_frontend lr_gambatte lr_fceumm lr_snes9x2005 lr_common)
zephyr_ld_options(-Wl,--wrap=__cxa_atexit)

target_sources(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/storage_shell.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/emu_shell.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/emu.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/menu.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gfx.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hud.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/input.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/storage.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/usbd_setup.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/resetinfo.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/battery.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/charger.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/settings.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/bootanim.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libretro/audio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libretro/state.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libretro/video.c
)
target_sources_ifdef(CONFIG_WIFI app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/wifi_prov.c)
target_sources_ifdef(CONFIG_HTTP_SERVER app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/webui.c)
if(CONFIG_HTTP_SERVER)
    # Firmware update: WiFi OTA + USB DFU share the PSRAM staging buffer
    # (src/ota.c) and the in-place installer.
    target_sources(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ota.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bl618/fw_install.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/libretro/workspace.c
    )
    target_sources_ifdef(CONFIG_USBD_DFU app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bl618/usb_dfu.c)
endif()
# The libc malloc arena lives in PSRAM — SRAM has no room left.
zephyr_code_relocate(FILES ${ZEPHYR_BASE}/lib/libc/common/source/stdlib/malloc.c
                     LOCATION PSRAM_NOINIT NOKEEP)
# NOTE: net_pkt.c pools CANNOT live in PSRAM — verified twice
# (2026-07-05, incl. a clean NOINIT-only test): the wifi blob DMAs
# frames directly into net_buf data; PSRAM is invisible to its
# engine and the chip hard-wedges on the first RX.
if(CONFIG_HTTP_SERVER)
    # Iterable section for the `webui` HTTP service's resources.
    zephyr_linker_sources(SECTIONS ${CMAKE_CURRENT_SOURCE_DIR}/src/webui-sections.ld)
    zephyr_linker_section(NAME http_resource_desc_webui KVMA RAM_REGION GROUP RODATA_REGION)
endif()

target_sources(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform_bl618.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/bl618/libgcc_ram.c
    # sys_arch_reboot(): Zephyr has none for BL61x.
    ${CMAKE_CURRENT_SOURCE_DIR}/src/bl618/reboot.c
)
# DFU / OTA payload: the whole-flash image (BFNP boot header + app)
# the BootROM boots, generated offline by bflb-mcu-tool.
set(DFU_BIN ${CMAKE_BINARY_DIR}/zephyr/zephyr.dfu.bin)
add_custom_command(
    OUTPUT ${DFU_BIN}
    COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/bflb_whole_img.py
            ${CMAKE_BINARY_DIR}/zephyr/zephyr.bin ${DFU_BIN}
    DEPENDS ${CMAKE_BINARY_DIR}/zephyr/zephyr.bin
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/bflb_whole_img.py
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/bflb_flash_support.py
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/flash/W25Q512JV_ef4020.conf
)
add_custom_target(dfu_image ALL DEPENDS ${DFU_BIN})
add_dependencies(dfu_image zephyr_final)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/external
)


# Convert PNG assets to RGB565 C headers at build time.
set(ASSET_DIR ${CMAKE_CURRENT_SOURCE_DIR}/assets)
set(ASSET_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/assets)
file(MAKE_DIRECTORY ${ASSET_GEN_DIR})
file(GLOB ASSET_PNGS ${ASSET_DIR}/*.png)
foreach(png ${ASSET_PNGS})
    get_filename_component(name ${png} NAME_WE)
    set(hdr ${ASSET_GEN_DIR}/${name}_icon.h)
    # Full-screen art caps at the 240px panel (only shrinks if larger);
    # everything else is icon-sized at 80.
    if(name STREQUAL "sleep")
        set(asset_max 240)
    else()
        set(asset_max 80)
    endif()
    add_custom_command(
        OUTPUT ${hdr}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/png2rgb565.py ${png} ${hdr} ${asset_max}
        DEPENDS ${png} ${CMAKE_CURRENT_SOURCE_DIR}/tools/png2rgb565.py
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/gif2frames.py
        COMMENT "asset: ${name}.png -> ${name}_icon.h"
    )
    list(APPEND ASSET_HEADERS ${hdr})
endforeach()
# Boot animation: GIF -> RGB565 frame arrays (one generated .c).
set(BOOT_ANIM_C ${ASSET_GEN_DIR}/boot_anim.c)
set(BOOT_ANIM_H ${ASSET_GEN_DIR}/boot_anim.h)
add_custom_command(
    OUTPUT ${BOOT_ANIM_C} ${BOOT_ANIM_H}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gif2frames.py
            ${ASSET_DIR}/boot.gif ${BOOT_ANIM_C} ${BOOT_ANIM_H}
    DEPENDS ${ASSET_DIR}/boot.gif ${CMAKE_CURRENT_SOURCE_DIR}/tools/gif2frames.py
    COMMENT "asset: boot.gif -> boot_anim.c/h"
)
target_sources(app PRIVATE ${BOOT_ANIM_C})
list(APPEND ASSET_HEADERS ${BOOT_ANIM_H})

add_custom_target(asset_headers DEPENDS ${ASSET_HEADERS})
add_dependencies(app asset_headers)

target_include_directories(app PRIVATE ${ASSET_GEN_DIR})

# Keep the video conversion and core callback hot paths optimized.
set_source_files_properties(src/emu.c src/libretro/video.c src/libretro/audio.c
    PROPERTIES COMPILE_OPTIONS "-O3")
