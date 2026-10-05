# AA's Assistant: optional effect plugin (Effects > Object > AA's Assistant).
# Builds <build>/plugins/out/aa_assistant/aa_assistant<suffix> plus this
# folder's README.md and screenshot.png; copy that folder into a paint.c
# plugins folder to install it (README.md).
pc_add_plugin(aa_assistant fxm_aa_assistant.c)
