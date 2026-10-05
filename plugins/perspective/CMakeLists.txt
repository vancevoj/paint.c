# Perspective: optional effect plugin (Effects > Distort > Perspective).
# Builds <build>/plugins/out/perspective/ with perspective<suffix>, this
# folder's README.md and screenshot.png; copy that folder into a paint.c
# plugins folder to install it (README.md).
pc_add_plugin(perspective fxm_perspective.c)
foreach(_f README.md screenshot.png)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/${_f} ${PC_PLUGIN_OUT}/perspective/${_f} COPYONLY)
endforeach()
