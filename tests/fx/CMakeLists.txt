# Lane L5 effect tests (L5a host runtime and adjustments, L5b and L5c
# effects). One executable and one CTest per test_*.c file in this
# directory; shared helpers live in fx_test_util.h (header-only).
# Threads::Threads covers C11 <threads.h> on glibc < 2.34 (thrd_create lives
# in libpthread there) and the pthread path of ThreadSanitizer builds.
find_package(Threads)
if(Threads_FOUND)
  pc_add_tests(pc_fx Threads::Threads)
else()
  pc_add_tests(pc_fx)
endif()
