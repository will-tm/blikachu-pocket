# Copyright (c) 2026 Camille BAUD
# SPDX-License-Identifier: Apache-2.0
# Source lists adapted from the supplied Zephyr libretro frontend and the
# pinned cores' Makefile.common files. Cores are explicit build targets;
# application linkage is owned by the frontend.

set(LR_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/deps/libretro)
set(LR_COMMON ${LR_ROOT}/common)

function(lr_allocator target)
  set(renames)
  foreach(allocator malloc calloc realloc reallocarray free memalign aligned_alloc posix_memalign strdup strndup)
    list(APPEND renames --redefine-sym ${allocator}=lr_${allocator})
  endforeach()
  # Rename archive references after compilation: defining malloc/free macros
  # breaks the C++ standard library's declarations in <cstdlib>.
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} ${renames} $<TARGET_FILE:${target}>
    COMMAND ${CMAKE_RANLIB} $<TARGET_FILE:${target}>
    VERBATIM)
endfunction()

function(lr_core target)
  add_library(${target} STATIC EXCLUDE_FROM_ALL ${ARGN})
  target_link_libraries(${target} PRIVATE zephyr_interface zephyr_generated_headers)
  target_compile_definitions(${target} PRIVATE
    _DEFAULT_SOURCE __LIBRETRO__ HAVE_STDINT_H HAVE_INTTYPES_H BLIKACHU_DMA_CHECKPOINT
    option_cats_us=${target}_option_cats_us
    option_defs_us=${target}_option_defs_us
    options_us=${target}_options_us)
  target_compile_options(${target} PRIVATE -fno-strict-aliasing)
  lr_allocator(${target})
  # Each core implements the same API; only the namespaced table is public.
  foreach(entry init deinit api_version get_system_info get_system_av_info
      set_environment set_video_refresh set_audio_sample set_audio_sample_batch
      set_input_poll set_input_state set_controller_port_device reset run
      serialize_size serialize unserialize cheat_reset cheat_set load_game
      load_game_special unload_game get_region get_memory_data get_memory_size)
    target_compile_definitions(${target} PRIVATE retro_${entry}=${target}_${entry})
  endforeach()
endfunction()

set(FCEU ${LR_ROOT}/fceumm/src)
file(GLOB FCEU_MAPPERS CONFIGURE_DEPENDS ${FCEU}/boards/*.c ${FCEU}/input/*.c)
set(FCEU_SRC)
foreach(source cart cheat crc32 fceu-endian fceu-memory fceu fds fds_apu
    file filter general ines input md5 nsf palette ppu sound state unif video vsuni x6502)
  list(APPEND FCEU_SRC ${FCEU}/${source}.c)
endforeach()
lr_core(lr_fceumm ${FCEU_SRC} ${FCEU_MAPPERS}
  ${FCEU}/drivers/libretro/libretro.c
  ${FCEU}/drivers/libretro/libretro_dipswitch.c)
target_include_directories(lr_fceumm PRIVATE ${FCEU}/drivers/libretro
  ${LR_COMMON}/include ${FCEU} ${FCEU}/input ${FCEU}/boards)
target_compile_definitions(lr_fceumm PRIVATE PATH_MAX=1024 INLINE=inline
  FCEU_VERSION_NUMERIC=9900 FRONTEND_SUPPORTS_RGB565 HAVE_NO_LANGEXTRA
  LSB_FIRST LOCAL_LE=1)

set(SNES ${LR_ROOT}/snes9x2005)
set(SNES_SRC)
foreach(source apu soundux spc700 c4 c4emu cheats cheats2 clip cpu cpuexec
    cpuops data dma dsp1 fxemu fxinst getset gfx globals memmap obc1 ppu sa1
    sa1cpu sdd1 sdd1emu seta seta010 seta011 seta018 spc7110 spc7110dec srtc tile)
  list(APPEND SNES_SRC ${SNES}/source/${source}.c)
endforeach()
lr_core(lr_snes9x2005 ${SNES}/libretro.c ${SNES_SRC}
  ${CMAKE_CURRENT_SOURCE_DIR}/src/libretro/snes_readonly.c)
target_include_directories(lr_snes9x2005 PRIVATE ${SNES} ${SNES}/source
  ${LR_COMMON}/include ${CMAKE_CURRENT_SOURCE_DIR}/src/libretro)
target_compile_definitions(lr_snes9x2005 PRIVATE LOAD_FROM_MEMORY RIGHTSHIFT_IS_SAR
  INLINE=inline NO_SPEEDHACKS SNES_READONLY_ROM PPU=lr_snes9x2005_PPU)

set(GB ${LR_ROOT}/gambatte)
set(GB_SRC)
foreach(source bootloader cpu gambatte initstate interrupter interruptrequester
    gambatte-memory sound statesaver tima video video_libretro mem/cartridge
    mem/cartridge_libretro mem/huc3 mem/memptrs mem/rtc sound/channel1
    sound/channel2 sound/channel3 sound/channel4 sound/duty_unit sound/envelope_unit
    sound/length_counter video/ly_counter video/lyc_irq video/next_m0_time
    video/ppu video/sprite_mapper)
  list(APPEND GB_SRC ${GB}/libgambatte/src/${source}.cpp)
endforeach()
lr_core(lr_gambatte ${GB_SRC} ${GB}/libgambatte/libretro/libretro.cpp
  ${GB}/libgambatte/libretro/gambatte_log.c ${GB}/libgambatte/libretro/blipper.c
  ${GB}/libgambatte/libretro/cc_resampler.c)
target_include_directories(lr_gambatte PRIVATE ${GB}/libgambatte/src
  ${GB}/libgambatte/include ${GB}/common ${GB}/common/resample
  ${GB}/libgambatte/libretro ${LR_COMMON}/include)
target_compile_definitions(lr_gambatte PRIVATE VIDEO_RGB565
  CC_RESAMPLER_NO_HIGHPASS HAVE_NO_LANGEXTRA)
target_compile_options(lr_gambatte PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)

set(COMMON_SRC)
foreach(source compat/compat_posix_string compat/compat_snprintf
    compat/compat_strcasestr compat/compat_strl compat/fopen_utf8
    encodings/encoding_utf file/file_path file/file_path_io streams/file_stream
    streams/file_stream_transforms streams/memory_stream string/stdstring time/rtime)
  list(APPEND COMMON_SRC ${LR_COMMON}/${source}.c)
endforeach()
add_library(lr_common STATIC EXCLUDE_FROM_ALL ${COMMON_SRC})
target_include_directories(lr_common PRIVATE ${LR_COMMON}/include)
target_compile_definitions(lr_common PRIVATE _DEFAULT_SOURCE PATH_MAX=1024)
target_link_libraries(lr_common PRIVATE zephyr_interface zephyr_generated_headers)
lr_allocator(lr_common)

add_library(lr_frontend STATIC EXCLUDE_FROM_ALL
  ${CMAKE_CURRENT_SOURCE_DIR}/src/libretro/core.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/libretro/content.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/libretro/memory.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/libretro/lifetime.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/libretro/session.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/libretro/environment.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/libretro/vfs.c)
target_include_directories(lr_frontend PRIVATE ${LR_COMMON}/include)
target_link_libraries(lr_frontend PRIVATE zephyr_interface zephyr_generated_headers)

add_custom_target(libretro_cores DEPENDS lr_gambatte lr_fceumm lr_snes9x2005 lr_common lr_frontend)
