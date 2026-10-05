# Lane L3 UI tests: one executable per test_*.c, run headless (software
# renderer on an SDL_Surface, dummy video driver for the clipboard).
pc_add_tests(pc_ui pc_ui_gallery)

# White-box tests also read toolkit internals (atlas, draw lists, the
# rasterizer); stb_truetype is a system include so its header stays quiet.
foreach(t test_ui_draw test_ui_raster)
  if(TARGET ${t})
    target_include_directories(${t} PRIVATE ${PROJECT_SOURCE_DIR}/src/ui)
    target_include_directories(${t} SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/stb_truetype)
  endif()
endforeach()

if(TARGET test_ui_gallery)
  target_compile_definitions(test_ui_gallery PRIVATE PC_UI_OUT_DIR="${CMAKE_BINARY_DIR}")
endif()
