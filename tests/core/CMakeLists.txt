# Reference suite (pc_tests) plus one executable per test_*.c file.
add_executable(pc_tests pc_tests.c)
target_link_libraries(pc_tests PRIVATE pc_core)
pc_warnings(pc_tests)
add_test(NAME pc_tests COMMAND pc_tests --quick)
# Keep ./build/pc_tests working as documented.
set_target_properties(pc_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

pc_add_tests(pc_core)
