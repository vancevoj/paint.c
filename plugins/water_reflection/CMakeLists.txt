# Water Reflection: optional effect plugin (Effects > Distort > Water
# Reflection). Builds <build>/plugins/out/water_reflection/ with
# water_reflection<suffix>, this folder's README.md and screenshot.png; copy
# that folder into a paint.c plugins folder to install it (README.md).
pc_add_plugin(water_reflection fxm_water_reflection.c)
foreach(_f README.md screenshot.png)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/${_f} ${PC_PLUGIN_OUT}/water_reflection/${_f}
                 COPYONLY)
endforeach()
