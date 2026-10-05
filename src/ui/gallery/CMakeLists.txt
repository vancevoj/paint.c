# Widget gallery: a library (shared with tests/ui screenshot tests) and an
# interactive executable that also renders headless screenshots. Built only
# with SDL (src/ui is skipped in PC_HEADLESS builds).
add_library(pc_ui_gallery STATIC ui_gallery.c)
target_include_directories(pc_ui_gallery PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pc_ui_gallery PUBLIC pc_ui)
pc_warnings(pc_ui_gallery)

add_executable(paintc_ui_gallery main.c)
target_link_libraries(paintc_ui_gallery PRIVATE pc_ui_gallery)
pc_warnings(paintc_ui_gallery)
# main() goes through SDL_main.h (UTF-8 argv on Windows); pc_sdl_main adds
# what MinGW needs for that (cmake/PcCommon.cmake, lane L0).
if(COMMAND pc_sdl_main)
  pc_sdl_main(paintc_ui_gallery)
endif()
