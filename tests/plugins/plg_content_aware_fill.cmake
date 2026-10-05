# Content Aware Fill (plugins/content_aware_fill, plugins lane 1): test_plg_content_aware_fill
# loads the built plugin through the editor's plugin loader (pc_app, so
# not in headless builds) and checks it through the fx host runtime.
if(TARGET plg_content_aware_fill AND TARGET pc_app)
  add_executable(test_plg_content_aware_fill ${CMAKE_CURRENT_LIST_DIR}/test_plg_content_aware_fill.c)
  target_include_directories(test_plg_content_aware_fill PRIVATE ${PROJECT_SOURCE_DIR}/tests
                             ${PROJECT_SOURCE_DIR}/tests/fx ${CMAKE_CURRENT_LIST_DIR})
  target_link_libraries(test_plg_content_aware_fill PRIVATE pc_app)
  if(Threads_FOUND)
    target_link_libraries(test_plg_content_aware_fill PRIVATE Threads::Threads)
  endif()
  pc_warnings(test_plg_content_aware_fill)
  add_dependencies(test_plg_content_aware_fill plg_content_aware_fill)
  target_compile_definitions(test_plg_content_aware_fill PRIVATE
                             PLG_PATH="$<TARGET_FILE:plg_content_aware_fill>")
  add_test(NAME test_plg_content_aware_fill COMMAND test_plg_content_aware_fill --quick)
  set_tests_properties(test_plg_content_aware_fill PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=dummy")
endif()
