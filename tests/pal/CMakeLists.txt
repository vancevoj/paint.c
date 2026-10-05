# Lane L0 tests: one executable per test_*.c (pc_add_tests in tests/).
pc_add_tests(pc_pal SDL3::SDL3)

# White-box access to the pure helpers in src/pal/pal_internal.h.
target_include_directories(test_pal PRIVATE ${PROJECT_SOURCE_DIR}/src/pal)
# main() goes through SDL_main.h for UTF-8 arguments on Windows.
pc_sdl_main(test_pal)
if(PC_TSAN)
  # Known lock-order report inside libdbus (see tsan.supp); the child
  # process of the single-instance test inherits the setting.
  set_tests_properties(test_pal PROPERTIES ENVIRONMENT
    "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan.supp:halt_on_error=1:second_deadlock_stack=1")
endif()

# Plugin fixture for pal_lib_*: a module next to the test executable, named
# like the plugins the app will load (no "lib" prefix, pal_lib_suffix()).
add_library(pal_test_plugin MODULE plugin_fixture.c)
target_include_directories(pal_test_plugin PRIVATE ${PROJECT_SOURCE_DIR}/include)
# Both land in this binary directory (per configuration with multi-config
# generators), which the test finds through pal_dir(PAL_DIR_EXE).
set_target_properties(pal_test_plugin PROPERTIES PREFIX "")
if(APPLE)
  set_target_properties(pal_test_plugin PROPERTIES SUFFIX ".dylib")
endif()
pc_warnings(pal_test_plugin)
add_dependencies(test_pal pal_test_plugin)

# lane UIB (wave 4 item 36): the clipboard retry policy test reads
# pal_internal.h too.
if(TARGET test_uib_clip)
  target_include_directories(test_uib_clip PRIVATE ${PROJECT_SOURCE_DIR}/src/pal)
endif()

# lane UIB (wave 4 item 43): tests that use the system clipboard never run at
# the same time (it is shared by every process of the session on Windows and
# under Wine); same rule as tests/app/CMakeLists.txt.
file(GLOB _pal_tests CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.c)
foreach(t ${_pal_tests})
  get_filename_component(_n ${t} NAME_WE)
  if(TARGET ${_n})
    file(STRINGS ${t} _clip REGEX "pal_clip_|SDL_[A-Za-z]*Clipboard" LIMIT_COUNT 1)
    if(_clip)
      set_property(TEST ${_n} APPEND PROPERTY RESOURCE_LOCK clipboard)
    endif()
  endif()
endforeach()
