target_sources(app PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/dfu_screen.c
  ${CMAKE_CURRENT_LIST_DIR}/uf2_disk.c
  ${CMAKE_CURRENT_LIST_DIR}/dfu_request.c
  ${CMAKE_CURRENT_LIST_DIR}/../src/bl618/reboot.c
  ${CMAKE_CURRENT_LIST_DIR}/../src/bl618/libgcc_ram.c
)
zephyr_include_directories(${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/../src)
zephyr_compile_definitions(BLIKACHU_RECOVERY=1)

set(DFU_ICON_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/assets)
file(MAKE_DIRECTORY ${DFU_ICON_DIR})
add_custom_command(
  OUTPUT ${DFU_ICON_DIR}/dfu_icon.h
  COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/../tools/png2rgb565.py
          ${CMAKE_CURRENT_LIST_DIR}/../assets/dfu.jpg ${DFU_ICON_DIR}/dfu_icon.h 96 --key-white
  DEPENDS ${CMAKE_CURRENT_LIST_DIR}/../assets/dfu.jpg
          ${CMAKE_CURRENT_LIST_DIR}/../tools/png2rgb565.py
          ${CMAKE_CURRENT_LIST_DIR}/../tools/gif2frames.py
)
add_custom_target(dfu_icon DEPENDS ${DFU_ICON_DIR}/dfu_icon.h)
add_dependencies(app dfu_icon)
target_include_directories(app PRIVATE ${DFU_ICON_DIR})
