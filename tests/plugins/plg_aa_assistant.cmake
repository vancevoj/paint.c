# AA's Assistant (plugins/aa_assistant, plugins lane 1): test_plg_aa_assistant
# loads the built plugin through the editor's plugin loader (pc_app, so
# not in headless builds) and checks it through the fx host runtime.
if(TARGET plg_aa_assistant AND TARGET pc_app)
  add_executable(test_plg_aa_assistant ${CMAKE_CURRENT_LIST_DIR}/test_plg_aa_assistant.c)
  target_include_directories(test_plg_aa_assistant PRIVATE ${PROJECT_SOURCE_DIR}/tests
                             ${PROJECT_SOURCE_DIR}/tests/fx ${CMAKE_CURRENT_LIST_DIR})
  target_link_libraries(test_plg_aa_assistant PRIVATE pc_app)
  if(Threads_FOUND)
    target_link_libraries(test_plg_aa_assistant PRIVATE Threads::Threads)
  endif()
  pc_warnings(test_plg_aa_assistant)
  add_dependencies(test_plg_aa_assistant plg_aa_assistant)
  target_compile_definitions(test_plg_aa_assistant PRIVATE
                             PLG_PATH="$<TARGET_FILE:plg_aa_assistant>")
  add_test(NAME test_plg_aa_assistant COMMAND test_plg_aa_assistant --quick)
  set_tests_properties(test_plg_aa_assistant PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=dummy")
endif()
