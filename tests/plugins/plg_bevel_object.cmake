# test_plg_bevel_object: the optional plugin of plugins/bevel_object, loaded as the
# built library through the real plugin loader (src/app/fx/afx_plugins.c)
# and run in the editor, so it needs pc_app (not built headless). Without
# PC_BUILD_PLUGINS the same source is built here, named like the plugin.
if(TARGET pc_app)
  set(_slug bevel_object)
  set(_t test_plg_${_slug})
  if(TARGET plg_${_slug})
    set(_plg plg_${_slug})
  else()
    set(_plg plgtest_${_slug})
    add_library(${_plg} MODULE ${PROJECT_SOURCE_DIR}/plugins/${_slug}/fxm_${_slug}.c)
    target_include_directories(${_plg} PRIVATE ${PROJECT_SOURCE_DIR}/include)
    set_target_properties(${_plg} PROPERTIES PREFIX "" OUTPUT_NAME ${_slug}
                          C_VISIBILITY_PRESET hidden
                          LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plg_${_slug}$<0:>"
                          RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plg_${_slug}$<0:>")
    if(APPLE)
      set_target_properties(${_plg} PROPERTIES SUFFIX ".dylib")
    elseif(WIN32)
      set_target_properties(${_plg} PROPERTIES SUFFIX ".dll")
    else()
      set_target_properties(${_plg} PROPERTIES SUFFIX ".so")
    endif()
    if(NOT MSVC)
      target_link_libraries(${_plg} PRIVATE m)
    endif()
    pc_warnings(${_plg})
  endif()
  add_executable(${_t} ${CMAKE_CURRENT_LIST_DIR}/${_t}.c)
  target_include_directories(${_t} PRIVATE ${PROJECT_SOURCE_DIR}/tests)
  target_link_libraries(${_t} PRIVATE pc_app)
  if(Threads_FOUND)
    target_link_libraries(${_t} PRIVATE Threads::Threads)
  endif()
  pc_warnings(${_t})
  add_dependencies(${_t} ${_plg})
  target_compile_definitions(${_t} PRIVATE PLG_PATH="$<TARGET_FILE:${_plg}>"
                             PLG_DIR="$<TARGET_FILE_DIR:${_plg}>")
  add_test(NAME ${_t} COMMAND ${_t} --quick WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  set_tests_properties(${_t} PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
                        LABELS "plugins;plugin_bevel_object")
endif()
