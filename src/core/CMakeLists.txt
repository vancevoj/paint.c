# Lane L1 (core). Headless C17, libc only (P-06). New files are picked up
# automatically.
pc_glob_sources(PC_CORE_SRCS ${CMAKE_CURRENT_SOURCE_DIR})
add_library(pc_core STATIC ${PC_CORE_SRCS})
target_include_directories(pc_core PUBLIC ${PROJECT_SOURCE_DIR}/include)
pc_warnings(pc_core)
if(NOT MSVC)
  target_link_libraries(pc_core PUBLIC m)
endif()
