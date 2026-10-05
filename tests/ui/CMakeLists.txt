# Lane L3 UI tests: one executable per test_*.c, run headless (software
# renderer on an SDL_Surface, dummy video driver for the clipboard).
pc_add_tests(pc_ui pc_ui_gallery)
if(TARGET test_ui_gallery)
  target_compile_definitions(test_ui_gallery PRIVATE PC_UI_OUT_DIR="${CMAKE_BINARY_DIR}")
endif()
