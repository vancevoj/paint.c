# Grid / Checkerboard plugin (plugins/grid_checkerboard):
# test_plg_grid_checkerboard loads the built plugin through the real plugin
# loader and checks the grid, checkerboard and dot geometry, the anchors,
# colors and alpha on synthetic images, tiling and thread invariance, and a
# run through the editor's dialog. Without PC_BUILD_PLUGINS the same source
# is built here, named like the plugin. Needs the editor library (not in
# headless builds).
if(TARGET pc_app)
  set(_s grid_checkerboard)
  if(TARGET plg_${_s})
    set(_plg plg_${_s})
  else()
    set(_plg plgtest_${_s})
    add_library(${_plg} MODULE ${PROJECT_SOURCE_DIR}/plugins/${_s}/fxm_${_s}.c)
    target_include_directories(${_plg} PRIVATE ${PROJECT_SOURCE_DIR}/include)
    set_target_properties(${_plg} PROPERTIES PREFIX "" OUTPUT_NAME ${_s} SUFFIX ${_fxp_suffix}
                          C_VISIBILITY_PRESET hidden
                          LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plg_${_s}$<0:>"
                          RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plg_${_s}$<0:>")
    if(NOT MSVC)
      target_link_libraries(${_plg} PRIVATE m)
    endif()
    pc_warnings(${_plg})
  endif()
  add_executable(test_plg_${_s} ${CMAKE_CURRENT_SOURCE_DIR}/test_plg_${_s}.c)
  target_include_directories(test_plg_${_s} PRIVATE ${PROJECT_SOURCE_DIR}/tests)
  target_link_libraries(test_plg_${_s} PRIVATE pc_app)
  if(Threads_FOUND)
    target_link_libraries(test_plg_${_s} PRIVATE Threads::Threads)
  endif()
  pc_warnings(test_plg_${_s})
  add_dependencies(test_plg_${_s} ${_plg})
  target_compile_definitions(test_plg_${_s} PRIVATE PLG_DIR="$<TARGET_FILE_DIR:${_plg}>")
  add_test(NAME test_plg_${_s} COMMAND test_plg_${_s} --quick
           WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  set_tests_properties(test_plg_${_s} PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
                        LABELS "plugins;plugin_grid_checkerboard")
endif()
