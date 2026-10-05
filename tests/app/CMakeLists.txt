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
