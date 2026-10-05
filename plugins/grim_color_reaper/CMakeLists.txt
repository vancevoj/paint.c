# Grim Color Reaper: optional effect plugin (Effects > Color > Grim Color
# Reaper). Builds <build>/plugins/out/grim_color_reaper/grim_color_reaper
# <suffix> plus this folder's README.md and screenshot; copy that folder
# into a paint.c plugins folder to install it (README.md).
pc_add_plugin(grim_color_reaper fxm_grim_color_reaper.c)
foreach(_f README.md screenshot.png)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${_f})
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/${_f} ${PC_PLUGIN_OUT}/grim_color_reaper/${_f}
                   COPYONLY)
  endif()
endforeach()
