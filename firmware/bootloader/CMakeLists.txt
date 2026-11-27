target_sources(app PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/dfu_screen.c
  ${CMAKE_CURRENT_LIST_DIR}/uf2_disk.c
  ${CMAKE_CURRENT_LIST_DIR}/dfu_request.c
  ${CMAKE_CURRENT_LIST_DIR}/../src/bl618/reboot.c
  ${CMAKE_CURRENT_LIST_DIR}/../src/bl618/libgcc_ram.c
)
zephyr_include_directories(${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/../src)
zephyr_compile_definitions(BLIKACHU_RECOVERY=1)
