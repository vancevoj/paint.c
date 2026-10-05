# Lane L2/L4: canvas rendering over SDL_Renderer (ADR-003). gfx_view.c is
# pure math (no SDL); gfx_canvas.c and gfx_ants.c draw with the renderer.
pc_glob_sources(PC_GFX_SRCS ${CMAKE_CURRENT_SOURCE_DIR})
add_library(pc_gfx STATIC ${PC_GFX_SRCS})
target_include_directories(pc_gfx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(pc_gfx PUBLIC pc_core SDL3::SDL3)
pc_warnings(pc_gfx)
if(NOT MSVC)
  target_link_libraries(pc_gfx PRIVATE m)
endif()
