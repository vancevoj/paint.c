# Golden comparisons against the non-authoritative Paint.NET 5.2 beta corpus
# in pdn52/ (ADR-016). Tests run with this directory as the working
# directory, so they open pdn52/input/... and pdn52/output_png/... directly.
# The PNG codec reads the files; pc_fx runs the effects; pc_core provides
# pc_composite_span for the blend modes.
find_package(Threads)
if(Threads_FOUND)
  pc_add_tests(pc_fx pc_codec Threads::Threads)
else()
  pc_add_tests(pc_fx pc_codec)
endif()
