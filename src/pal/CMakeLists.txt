# Lane L0 (platform layer). pal.h v1 over SDL3 plus small native shims:
# pal_common.c, pal_pool.c, pal_path.c and pal_clip.c are portable;
# pal_posix.c and pal_win32.c each compile to nothing on the other OS
# family. No Objective-C is needed on macOS.
pc_glob_sources(PC_PAL_SRCS ${CMAKE_CURRENT_SOURCE_DIR})
add_library(pc_pal STATIC ${PC_PAL_SRCS} ${CMAKE_CURRENT_SOURCE_DIR}/pal_internal.h)
target_include_directories(pc_pal PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(pc_pal PUBLIC pc_core SDL3::SDL3)
if(WIN32)
  # shell32: SHGetFolderPathW, SHOpenFolderAndSelectItems, ShellExecuteW
  # ole32: CoInitializeEx for the shell calls; user32: clipboard
  target_link_libraries(pc_pal PUBLIC shell32 ole32 user32)
else()
  target_link_libraries(pc_pal PUBLIC ${CMAKE_DL_LIBS})
endif()
pc_warnings(pc_pal)
