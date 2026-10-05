# Shape3D: optional effect plugin (Effects > Render > Shape3D).
# Builds <build>/plugins/out/shape3d/shape3d<suffix> plus this folder's
# README.md and screenshot.png; copy that folder into a paint.c plugins
# folder to install it (README.md).
pc_add_plugin(shape3d fxm_shape3d.c)
foreach(_f README.md screenshot.png)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/${_f} ${PC_PLUGIN_OUT}/shape3d/${_f} COPYONLY)
endforeach()
