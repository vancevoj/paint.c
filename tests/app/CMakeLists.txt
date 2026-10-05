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
