# Optional effect plugins (separate downloads, not part of paintc).
# Every plugins/<name>/ directory with a CMakeLists.txt is added; it calls
#   pc_add_plugin(<name> <sources...>)
# which builds a MODULE named like a real plugin (no "lib" prefix, .so/.dll/
# .dylib) against include/fx only, into <build>/plugins/out/<name>/.
# Copy that folder into the plugins folder (Linux ~/.local/share/paintc/plugins,
# Windows %APPDATA%\paintc\plugins, macOS ~/Library/Application Support/
# paint.c/plugins, or a "plugins" folder next to paintc) to install it.
if(APPLE)
  set(PC_PLUGIN_SUFFIX .dylib)
elseif(WIN32)
  set(PC_PLUGIN_SUFFIX .dll)
else()
  set(PC_PLUGIN_SUFFIX .so)
endif()
set(PC_PLUGIN_OUT ${CMAKE_BINARY_DIR}/plugins/out)

function(pc_add_plugin name)
  add_library(plg_${name} MODULE ${ARGN})
  target_include_directories(plg_${name} PRIVATE ${PROJECT_SOURCE_DIR}/include
                             ${CMAKE_CURRENT_SOURCE_DIR})
  set_target_properties(plg_${name} PROPERTIES OUTPUT_NAME ${name} PREFIX ""
                        SUFFIX ${PC_PLUGIN_SUFFIX} C_VISIBILITY_PRESET hidden
                        LIBRARY_OUTPUT_DIRECTORY "${PC_PLUGIN_OUT}/${name}$<0:>"
                        RUNTIME_OUTPUT_DIRECTORY "${PC_PLUGIN_OUT}/${name}$<0:>")
  if(NOT MSVC)
    target_link_libraries(plg_${name} PRIVATE m)
  endif()
  pc_warnings(plg_${name})
  set_property(GLOBAL APPEND PROPERTY PC_PLUGIN_TARGETS plg_${name})
endfunction()

file(GLOB _plugin_dirs LIST_DIRECTORIES true ${CMAKE_CURRENT_SOURCE_DIR}/*)
list(SORT _plugin_dirs)
foreach(d ${_plugin_dirs})
  if(IS_DIRECTORY ${d} AND EXISTS ${d}/CMakeLists.txt)
    add_subdirectory(${d})
  endif()
endforeach()

get_property(_all GLOBAL PROPERTY PC_PLUGIN_TARGETS)
if(_all)
  add_custom_target(plugins DEPENDS ${_all})
endif()
