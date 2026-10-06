# Align Object: optional effect plugin (Effects > Object > Align Object).
# Builds <build>/plugins/out/align_object/align_object<suffix> plus this
# folder's README.md and screenshot.png; copy that folder into a paint.c
# plugins folder to install it (README.md).
pc_add_plugin(align_object fxm_align_object.c)
