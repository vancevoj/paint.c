# Shape3D (plugins/shape3d, plugins lane 1): test_plg_shape3d
# loads the built plugin through the editor's plugin loader (pc_app, so
# not in headless builds) and checks it through the fx host runtime.
if(TARGET plg_shape3d AND TARGET pc_app)
  add_executable(test_plg_shape3d ${CMAKE_CURRENT_LIST_DIR}/test_plg_shape3d.c)
  target_include_directories(test_plg_shape3d PRIVATE ${PROJECT_SOURCE_DIR}/tests
                             ${PROJECT_SOURCE_DIR}/tests/fx ${CMAKE_CURRENT_LIST_DIR})
  target_link_libraries(test_plg_shape3d PRIVATE pc_app)
  if(Threads_FOUND)
    target_link_libraries(test_plg_shape3d PRIVATE Threads::Threads)
  endif()
  pc_warnings(test_plg_shape3d)
  add_dependencies(test_plg_shape3d plg_shape3d)
  target_compile_definitions(test_plg_shape3d PRIVATE
                             PLG_PATH="$<TARGET_FILE:plg_shape3d>")
  add_test(NAME test_plg_shape3d COMMAND test_plg_shape3d --quick)
  set_tests_properties(test_plg_shape3d PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
                        LABELS "plugins;plugin_shape3d")
endif()
