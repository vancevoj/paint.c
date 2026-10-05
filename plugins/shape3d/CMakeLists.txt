# Shape3D: optional effect plugin (Effects > Render > Shape3D).
# Builds <build>/plugins/out/shape3d/shape3d<suffix> plus this folder's
# README.md and screenshot.png; copy that folder into a paint.c plugins
# folder to install it (README.md).
pc_add_plugin(shape3d fxm_shape3d.c)
