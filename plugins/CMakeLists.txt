# Optional effect plugins (separate downloads, not part of paintc).
# Every plugins/<name>/ directory with a CMakeLists.txt is added; it calls
#   pc_add_plugin(<name> <sources...>)
# which builds a MODULE named like a real plugin (no "lib" prefix, .so/.dll/
# .dylib) against include/fx only, into <build>/plugins/out/<name>/, and
# copies the folder's README.md, screenshot.png and LICENSE-original.txt
# (those that exist) next to the library, so that folder is what a user
# installs and what packaging/plugins/build-plugins.sh zips.
# Copy that folder into the plugins folder (Linux ~/.local/share/paintc/plugins,
# Windows %APPDATA%\paintc\plugins, macOS ~/Library/Application Support/
# paint.c/plugins, or a "plugins" folder next to paintc) to install it.
# Conventions for a plugin folder, its tests (tests/plugins/plg_<name>.cmake
# with pc_plugin_test) and its README card: plugins/README.md.
if(APPLE)
  set(PC_PLUGIN_SUFFIX .dylib)
elseif(WIN32)
  set(PC_PLUGIN_SUFFIX .dll)
else()
  set(PC_PLUGIN_SUFFIX .so)
endif()
set(PC_PLUGIN_OUT ${CMAKE_BINARY_DIR}/plugins/out)
# tests/plugins is a sibling directory scope: it reads these globals.
set_property(GLOBAL PROPERTY PC_PLUGIN_OUT_DIR ${PC_PLUGIN_OUT})
set_property(GLOBAL PROPERTY PC_PLUGIN_SUFFIX ${PC_PLUGIN_SUFFIX})

# Files of a plugin folder that ship next to the library.
set(PC_PLUGIN_SHIP_FILES README.md screenshot.png LICENSE-original.txt)

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
  # configure_file reruns CMake when one of them changes.
  foreach(f ${PC_PLUGIN_SHIP_FILES})
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${f})
      configure_file(${CMAKE_CURRENT_SOURCE_DIR}/${f} ${PC_PLUGIN_OUT}/${name}/${f} COPYONLY)
    endif()
  endforeach()
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
