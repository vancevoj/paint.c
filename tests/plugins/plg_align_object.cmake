# Align Object (plugins/align_object): every position through the generic
# plugin checks, with the built library loaded by the real loader. Its
# render logic is also tested in tests/fx/test_fx2_align_object.c and its
# dialog in tests/app/test_align_dialog.c.
pc_plugin_test(align_object test_plg_align_object.c)
