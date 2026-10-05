# Lane L3: immediate-mode UI toolkit over SDL_Renderer (ADR-003, ADR-004).
# pc_ui depends on SDL3 and the header-only parts of pc_core, never on pal.
include(PcEmbed)

pc_glob_sources(PC_UI_SRCS ${CMAKE_CURRENT_SOURCE_DIR})
list(REMOVE_ITEM PC_UI_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/ui_stb.c)

set(PC_UI_EMBED)
pc_embed_file(PC_UI_EMBED ${PROJECT_SOURCE_DIR}/assets/fonts/Inter-Regular.ttf
              ui_font_data_inter_regular)
pc_embed_file(PC_UI_EMBED ${PROJECT_SOURCE_DIR}/assets/fonts/Inter-SemiBold.ttf
              ui_font_data_inter_semibold)

# stb_truetype implementation: third-party code, built without -Werror.
add_library(pc_ui_stb STATIC ui_stb.c)
target_include_directories(pc_ui_stb PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(pc_ui_stb SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/stb_truetype)
if(NOT MSVC)
  target_link_libraries(pc_ui_stb PRIVATE m)
endif()

add_library(pc_ui STATIC ${PC_UI_SRCS} ${PC_UI_EMBED})
target_include_directories(pc_ui PUBLIC ${PROJECT_SOURCE_DIR}/include
                                 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(pc_ui SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/stb_truetype)
target_link_libraries(pc_ui PUBLIC SDL3::SDL3 pc_core PRIVATE pc_ui_stb)
pc_warnings(pc_ui)
if(NOT MSVC)
  target_link_libraries(pc_ui PRIVATE m)
endif()

add_subdirectory(gallery)
