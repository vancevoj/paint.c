# Each tests/<lane>/ directory with a CMakeLists.txt is added automatically.
# Lanes own their test directory. tests/pc_test.h is the shared harness.

# pc_add_tests(<lib> [extra libs...]) - one executable and one CTest per
# test_*.c file in the calling directory. Tests run with --quick under CTest.
function(pc_add_tests)
  file(GLOB _tests CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.c)
  list(SORT _tests)
  foreach(t ${_tests})
    get_filename_component(_n ${t} NAME_WE)
    add_executable(${_n} ${t})
    target_include_directories(${_n} PRIVATE ${PROJECT_SOURCE_DIR}/tests)
    target_link_libraries(${_n} PRIVATE ${ARGN})
    pc_warnings(${_n})
    add_test(NAME ${_n} COMMAND ${_n} --quick
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  endforeach()
endfunction()

file(GLOB _children LIST_DIRECTORIES true ${CMAKE_CURRENT_SOURCE_DIR}/*)
list(SORT _children)
foreach(child ${_children})
  if(IS_DIRECTORY ${child} AND EXISTS ${child}/CMakeLists.txt)
    get_filename_component(_name ${child} NAME)
    if(PC_HEADLESS AND _name MATCHES "^(pal|ui|gfx|app)$")
      continue()
    endif()
    add_subdirectory(${child})
  endif()
endforeach()

