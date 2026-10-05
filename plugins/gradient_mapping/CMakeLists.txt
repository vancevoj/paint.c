# Gradient Mapping: optional adjustment plugin (Adjustments > Gradient
# Mapping), ported from pyrochild's MIT licensed plugin. Builds
# <build>/plugins/out/gradient_mapping/gradient_mapping<suffix> plus this
# folder's README.md, screenshot and the original MIT license
# (LICENSE-original.txt); copy that folder into a paint.c plugins folder to
# install it (README.md).
pc_add_plugin(gradient_mapping fxm_gradient_mapping.c)
foreach(_f README.md screenshot.png LICENSE-original.txt)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${_f})
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/${_f} ${PC_PLUGIN_OUT}/gradient_mapping/${_f}
                   COPYONLY)
  endif()
endforeach()
