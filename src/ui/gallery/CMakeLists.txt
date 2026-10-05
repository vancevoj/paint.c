# Widget gallery: a library (shared with tests/ui screenshot tests) and an
# interactive executable that also renders headless screenshots.
add_library(pc_ui_gallery STATIC ui_gallery.c)
target_include_directories(pc_ui_gallery PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pc_ui_gallery PUBLIC pc_ui)
pc_warnings(pc_ui_gallery)

add_executable(paintc_ui_gallery main.c)
target_link_libraries(paintc_ui_gallery PRIVATE pc_ui_gallery)
pc_warnings(paintc_ui_gallery)
