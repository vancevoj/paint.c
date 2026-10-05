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

# lane UIB (wave 4 item 43, marked edit in lane L3's file): text field copy
# and paste go through SDL's clipboard, which every process of the session
# shares on Windows and under Wine; such tests take the same CTest lock as
# tests/app and tests/pal so `ctest -j` never runs two of them at once.
file(GLOB _ui_tests CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.c)
foreach(t ${_ui_tests})
  get_filename_component(_n ${t} NAME_WE)
  if(TARGET ${_n})
    file(STRINGS ${t} _clip REGEX "[Cc]lipboard" LIMIT_COUNT 1)
    if(_clip)
      set_property(TEST ${_n} APPEND PROPERTY RESOURCE_LOCK clipboard)
    endif()
  endif()
endforeach()
