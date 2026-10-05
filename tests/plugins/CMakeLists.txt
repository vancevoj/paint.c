# Lane F: effect plugin fixtures for tests/app/test_f_plugins.c (one valid
# sample, a second valid plugin in a subfolder and seven malformed ones,
# plus a file that is not a library). Each fixture is a MODULE named like a
# real plugin (no "lib" prefix, pal_lib_suffix()) built into
#   <build>/tests/plugins/fixtures/good/            valid plugins
#   <build>/tests/plugins/fixtures/good/nested/     one level down
#   <build>/tests/plugins/fixtures/bad/             malformed plugins
# The generator expression keeps multi-config generators from adding a
# per-configuration subfolder.
set(_fxp_out ${CMAKE_CURRENT_BINARY_DIR}/fixtures)
if(APPLE)
  set(_fxp_suffix .dylib)
elseif(WIN32)
  set(_fxp_suffix .dll)
else()
  set(_fxp_suffix .so)
endif()
set(_fxp_targets)

function(pc_fx_fixture name src subdir)
  add_library(${name} MODULE ${src})
  target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/include)
  set_target_properties(${name} PROPERTIES PREFIX "" SUFFIX ${_fxp_suffix}
                        LIBRARY_OUTPUT_DIRECTORY "${_fxp_out}/${subdir}$<0:>"
                        RUNTIME_OUTPUT_DIRECTORY "${_fxp_out}/${subdir}$<0:>")
  if(NOT MSVC)
    target_link_libraries(${name} PRIVATE m)
  endif()
  pc_warnings(${name})
  set(_fxp_targets ${_fxp_targets} ${name} PARENT_SCOPE)
endfunction()

pc_fx_fixture(fxp_sample sample_plugin.c good)
pc_fx_fixture(fxp_swap sample_swap.c good/nested)
foreach(k 1 2 3 4 5 6 7)
  pc_fx_fixture(fxp_bad${k} bad_plugins.c bad)
  target_compile_definitions(fxp_bad${k} PRIVATE BAD_KIND=${k})
endforeach()
file(WRITE ${_fxp_out}/bad/garbage${_fxp_suffix} "this is not a shared library\n")

if(TARGET test_f_plugins)
  add_dependencies(test_f_plugins ${_fxp_targets})
  target_compile_definitions(test_f_plugins PRIVATE AFX_FIXTURES="${_fxp_out}"
                             AFX_SUFFIX="${_fxp_suffix}")
endif()

# The minimal plugin of docs/PLUGINS.md (plugin_meta.py check keeps the two
# identical), built like the page says: include/fx only, hidden visibility,
# output darken<suffix>. test_plg_all loads and checks it.
pc_fx_fixture(fxp_example minimal_plugin.c example)
target_include_directories(fxp_example PRIVATE ${PROJECT_SOURCE_DIR}/include/fx)
set_target_properties(fxp_example PROPERTIES OUTPUT_NAME darken C_VISIBILITY_PRESET hidden)

# ---- tests of the optional plugins in plugins/<slug>/ ------------------------
# (plugins/README.md, docs/PLUGINS.md)
#
# pc_plugin_test(<slug> <test source> [<more sources>...])
#   One executable named after <test source> (test_plg_<slug>.c by
#   convention) and one CTest entry (run with --quick, label "plugins"),
#   built after the plugin target plg_<slug> and linked with pc_app (the
#   real plugin loader, src/app/fx/afx.h) and pc_fx. Include paths: tests/
#   (pc_test.h), tests/fx (fx_test_util.h) and tests/plugins
#   (plg_test_util.h). Compile definitions:
#     PC_PLUGIN_SLUG        "<slug>"
#     PC_PLUGIN_PATH        the built library, <build>/plugins/out/<slug>/<slug><suffix>
#     PC_PLUGIN_DIR         its folder
#     PC_PLUGIN_SOURCE_DIR  plugins/<slug> in the source tree
#   Skipped with a status message in headless builds (no pc_app) and when
#   the plugin is not built (PC_BUILD_PLUGINS=OFF).
#
# Each plugin lane adds tests/plugins/plg_<slug>.cmake (included below, in
# name order) with its pc_plugin_test calls, so no lane edits this file.
function(pc_plugin_test slug src)
  get_filename_component(_n ${src} NAME_WE)
  if(NOT TARGET pc_app)
    message(STATUS "paint.c: ${_n} skipped (headless build: no plugin loader)")
    return()
  endif()
  if(NOT TARGET plg_${slug})
    message(STATUS "paint.c: ${_n} skipped (plugin ${slug} is not built)")
    return()
  endif()
  add_executable(${_n} ${src} ${ARGN})
  target_include_directories(${_n} PRIVATE ${PROJECT_SOURCE_DIR}/tests
                             ${PROJECT_SOURCE_DIR}/tests/fx ${PROJECT_SOURCE_DIR}/tests/plugins)
  target_link_libraries(${_n} PRIVATE pc_app pc_fx)
  if(Threads_FOUND)
    target_link_libraries(${_n} PRIVATE Threads::Threads)
  endif()
  pc_warnings(${_n})
  add_dependencies(${_n} plg_${slug})
  target_compile_definitions(${_n} PRIVATE PC_PLUGIN_SLUG="${slug}"
                             PC_PLUGIN_PATH="$<TARGET_FILE:plg_${slug}>"
                             PC_PLUGIN_DIR="$<TARGET_FILE_DIR:plg_${slug}>"
                             PC_PLUGIN_SOURCE_DIR="${PROJECT_SOURCE_DIR}/plugins/${slug}")
  add_test(NAME ${_n} COMMAND ${_n} --quick WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  set_tests_properties(${_n} PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
                       LABELS "plugins;plugin_${slug}")
endfunction()

# test_plg_all: every folder of <build>/plugins/out, loaded through the real
# loader, every effect with default parameters through the effect runner.
get_property(_plg_targets GLOBAL PROPERTY PC_PLUGIN_TARGETS)
get_property(_plg_out GLOBAL PROPERTY PC_PLUGIN_OUT_DIR)
if(TARGET pc_app AND _plg_targets)
  set(_plg_slugs)
  foreach(t ${_plg_targets})
    string(REGEX REPLACE "^plg_" "" _s ${t})
    list(APPEND _plg_slugs ${_s})
  endforeach()
  list(SORT _plg_slugs)
  string(REPLACE ";" "," _plg_slug_csv "${_plg_slugs}")
  add_executable(test_plg_all test_plg_all.c)
  target_include_directories(test_plg_all PRIVATE ${PROJECT_SOURCE_DIR}/tests
                             ${PROJECT_SOURCE_DIR}/tests/fx ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(test_plg_all PRIVATE pc_app pc_fx)
  if(Threads_FOUND)
    target_link_libraries(test_plg_all PRIVATE Threads::Threads)
  endif()
  pc_warnings(test_plg_all)
  add_dependencies(test_plg_all ${_plg_targets} fxp_example)
  target_compile_definitions(test_plg_all PRIVATE PC_PLUGIN_OUT_DIR="${_plg_out}"
                             PC_PLUGIN_SLUGS="${_plg_slug_csv}"
                             PC_PLUGIN_SOURCE_ROOT="${PROJECT_SOURCE_DIR}/plugins"
                             PC_PLUGIN_RELEASE_CONF="${PROJECT_SOURCE_DIR}/packaging/plugins/release.conf"
                             PC_PLUGIN_EXAMPLE="$<TARGET_FILE:fxp_example>")
  add_test(NAME test_plg_all COMMAND test_plg_all --quick
           WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  set_tests_properties(test_plg_all PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
                       LABELS "plugins")
elseif(NOT TARGET pc_app)
  message(STATUS "paint.c: test_plg_all skipped (headless build: no plugin loader)")
endif()

# Every plg_<slug>.cmake of this folder (one per plugin lane), name order.
file(GLOB _plg_cmake_files CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/plg_*.cmake)
list(SORT _plg_cmake_files)
foreach(_plg_cmake_file ${_plg_cmake_files})
  include(${_plg_cmake_file})
endforeach()

# The README card and docs tables of every plugin (packaging/plugins/
# plugin_meta.py check): docs/PLUGINS.md and README.md must list exactly the
# plugins of plugins/ with their current cards.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME test_plg_docs
           COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/packaging/plugins/plugin_meta.py
                   check)
  set_tests_properties(test_plg_docs PROPERTIES LABELS "plugins")
endif()
