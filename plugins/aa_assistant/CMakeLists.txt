# AA's Assistant: optional effect plugin (Effects > Object > AA's Assistant).
# Builds <build>/plugins/out/aa_assistant/aa_assistant<suffix> plus this
# folder's README.md and screenshot.png; copy that folder into a paint.c
# plugins folder to install it (README.md).
pc_add_plugin(aa_assistant fxm_aa_assistant.c)
foreach(_f README.md screenshot.png)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/${_f} ${PC_PLUGIN_OUT}/aa_assistant/${_f} COPYONLY)
endforeach()
