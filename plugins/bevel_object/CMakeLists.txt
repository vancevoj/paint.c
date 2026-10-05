# Bevel Object: optional effect plugin (Effects > Object > Bevel Object).
# Builds <build>/plugins/out/bevel_object/bevel_object<suffix> plus this
# folder's README.md and screenshot; copy that folder into a paint.c plugins
# folder to install it (README.md).
pc_add_plugin(bevel_object fxm_bevel_object.c)
foreach(_f README.md screenshot.png)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${_f})
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/${_f} ${PC_PLUGIN_OUT}/bevel_object/${_f} COPYONLY)
  endif()
endforeach()
