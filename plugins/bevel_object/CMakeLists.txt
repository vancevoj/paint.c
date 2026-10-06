# Bevel Object: optional effect plugin (Effects > Object > Bevel Object).
# Builds <build>/plugins/out/bevel_object/bevel_object<suffix> plus this
# folder's README.md and screenshot; copy that folder into a paint.c plugins
# folder to install it (README.md).
pc_add_plugin(bevel_object fxm_bevel_object.c)
