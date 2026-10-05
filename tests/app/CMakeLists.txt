# Lane L2/L4 tests: one executable per test_*.c, run headless (software
# renderer on an off-screen surface, dummy video driver for SDL services).
pc_add_tests(pc_app)
file(GLOB _app_tests CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.c)
foreach(t ${_app_tests})
  get_filename_component(_n ${t} NAME_WE)
  if(TARGET ${_n})
    set_tests_properties(${_n} PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=dummy")
    target_compile_definitions(${_n} PRIVATE PC_APP_TEST_OUT_DIR="${CMAKE_BINARY_DIR}")
  endif()
endforeach()

# The executable's own end-to-end check (open, paint, undo, redo, save,
# reload) through main(), headless.
if(TARGET paintc)
  add_test(NAME paintc_self_test COMMAND paintc --self-test --headless)
  set_tests_properties(paintc_self_test PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=dummy")
endif()

# Lane I: the crash recovery test starts paintc itself (kill -9 and restart).
if(TARGET paintc AND TARGET test_i_recovery)
  target_compile_definitions(test_i_recovery PRIVATE PC_PAINTC_EXE="$<TARGET_FILE:paintc>")
  add_dependencies(test_i_recovery paintc)
endif()

# lane KEYS: the command line tests start paintc itself (--diagnostics
# without a video driver, --set).
if(TARGET paintc AND TARGET test_keys_cli)
  target_compile_definitions(test_keys_cli PRIVATE PC_PAINTC_EXE="$<TARGET_FILE:paintc>")
  add_dependencies(test_keys_cli paintc)
endif()

# lane UIB (wave 4 item 9): the --set forwarding test starts paintc itself
# (a primary instance and later launches).
if(TARGET paintc AND TARGET test_uib_cli)
  target_compile_definitions(test_uib_cli PRIVATE PC_PAINTC_EXE="$<TARGET_FILE:paintc>")
  add_dependencies(test_uib_cli paintc)
endif()

# lane UIB (wave 4 item 43): tests that put data on or take data from the
# system clipboard never run at the same time. The clipboard is shared by
# every process of a desktop session (Windows, and Wine running the mingw
# build), so `ctest -j` let one test empty or hold it while another read it.
# The list is found by scanning the sources for clipboard calls, so new
# tests join it automatically. Keep the pattern in sync with
# tests/pal/CMakeLists.txt and tests/ui/CMakeLists.txt.
set(PC_CLIPBOARD_TEST_REGEX
    "pal_clip_|SDL_[A-Za-z]*Clipboard|edit\\.(cut|copy|paste)|m_paste_|Copy Path")
foreach(t ${_app_tests})
  get_filename_component(_n ${t} NAME_WE)
  if(TARGET ${_n})
    file(STRINGS ${t} _clip REGEX "${PC_CLIPBOARD_TEST_REGEX}" LIMIT_COUNT 1)
    if(_clip)
      set_property(TEST ${_n} APPEND PROPERTY RESOURCE_LOCK clipboard)
    endif()
  endif()
endforeach()

# lane UIB (wave 4 items 4 to 6): the user guide test builds the guide's
# plugin example (docs/help/fade_plugin.c) like a user would, as a module
# named fade<suffix>, and loads it through the real plugin loader.
if(TARGET test_uib_help)
  add_library(uib_fade_plugin MODULE ${PROJECT_SOURCE_DIR}/docs/help/fade_plugin.c)
  target_include_directories(uib_fade_plugin PRIVATE ${PROJECT_SOURCE_DIR}/include/fx)
  set_target_properties(uib_fade_plugin PROPERTIES PREFIX "" OUTPUT_NAME fade
                        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/uib_plugin$<0:>"
                        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/uib_plugin$<0:>")
  if(APPLE)
    set_target_properties(uib_fade_plugin PROPERTIES SUFFIX ".dylib")
  endif()
  pc_warnings(uib_fade_plugin)
  add_dependencies(test_uib_help uib_fade_plugin)
  target_compile_definitions(test_uib_help PRIVATE
                             UIB_FADE_PLUGIN="$<TARGET_FILE:uib_fade_plugin>")
endif()
# Help > Forum exists only when PC_PROJECT_PUBLIC is on (src/app/CMakeLists.txt).
foreach(_n test_uib_help test_m_help test_keys_menu)
  if(TARGET ${_n})
    target_compile_definitions(${_n} PRIVATE PC_PROJECT_PUBLIC=$<BOOL:${PC_PROJECT_PUBLIC}>)
  endif()
endforeach()

# Align Object (ADR-024): the dialog test loads the optional plugin of
# plugins/align_object through the real plugin loader. Without
# PC_BUILD_PLUGINS the same source is built here, named like the plugin.
if(TARGET test_align_dialog)
  if(TARGET plg_align_object)
    set(_align_plugin plg_align_object)
  else()
    set(_align_plugin align_object_test_plugin)
    add_library(${_align_plugin} MODULE ${PROJECT_SOURCE_DIR}/plugins/align_object/fxm_align_object.c)
    target_include_directories(${_align_plugin} PRIVATE ${PROJECT_SOURCE_DIR}/include)
    set_target_properties(${_align_plugin} PROPERTIES PREFIX "" OUTPUT_NAME align_object
                          C_VISIBILITY_PRESET hidden
                          LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/align_plugin$<0:>"
                          RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/align_plugin$<0:>")
    if(APPLE)
      set_target_properties(${_align_plugin} PROPERTIES SUFFIX ".dylib")
    elseif(WIN32)
      set_target_properties(${_align_plugin} PROPERTIES SUFFIX ".dll")
    else()
      set_target_properties(${_align_plugin} PROPERTIES SUFFIX ".so")
    endif()
    if(NOT MSVC)
      target_link_libraries(${_align_plugin} PRIVATE m)
    endif()
    pc_warnings(${_align_plugin})
  endif()
  add_dependencies(test_align_dialog ${_align_plugin})
  target_compile_definitions(test_align_dialog PRIVATE
                             ALIGN_PLUGIN_DIR="$<TARGET_FILE_DIR:${_align_plugin}>")
endif()
