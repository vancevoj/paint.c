# Reference core for the portable Paint.NET-class editor ("pcanvas").
cmake_minimum_required(VERSION 3.20)
project(pcanvas_refcore C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(PC_SANITIZE "Build with AddressSanitizer + UBSan (GCC/Clang)" OFF)

add_library(pc_core STATIC
    src/pc_mem.c src/pc_blend.c src/pc_tile.c src/pc_doc.c
    src/pc_hist.c src/pc_txn.c src/pc_fill.c)
target_include_directories(pc_core PUBLIC include)

add_executable(pc_tests tests/pc_tests.c)
target_link_libraries(pc_tests PRIVATE pc_core)

if(MSVC)
  target_compile_options(pc_core PRIVATE /W4 /WX)
  target_compile_options(pc_tests PRIVATE /W4)
else()
  set(PC_WARN -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Werror)
  target_compile_options(pc_core PRIVATE ${PC_WARN})
  target_compile_options(pc_tests PRIVATE ${PC_WARN})
  target_link_libraries(pc_tests PRIVATE m)
  if(PC_SANITIZE)
    set(PC_SAN -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_compile_options(pc_core PUBLIC ${PC_SAN})
    target_link_options(pc_core PUBLIC ${PC_SAN})
  endif()
endif()

enable_testing()
add_test(NAME pc_tests COMMAND pc_tests --quick)
