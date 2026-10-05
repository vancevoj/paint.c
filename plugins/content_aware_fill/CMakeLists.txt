# Content Aware Fill: optional effect plugin (Effects > Selection > Content
# Aware Fill). Builds <build>/plugins/out/content_aware_fill/
# content_aware_fill<suffix> plus this folder's README.md and
# screenshot.png; copy that folder into a paint.c plugins folder to install
# it (README.md).
pc_add_plugin(content_aware_fill fxm_content_aware_fill.c)
foreach(_f README.md screenshot.png)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/${_f} ${PC_PLUGIN_OUT}/content_aware_fill/${_f}
                 COPYONLY)
endforeach()
