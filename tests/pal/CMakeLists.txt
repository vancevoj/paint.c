# Lane L0 tests: one executable per test_*.c (pc_add_tests in tests/).
pc_add_tests(pc_pal SDL3::SDL3)

# White-box access to the pure helpers in src/pal/pal_internal.h.
target_include_directories(test_pal PRIVATE ${PROJECT_SOURCE_DIR}/src/pal)
# main() goes through SDL_main.h for UTF-8 arguments on Windows.
pc_sdl_main(test_pal)

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
