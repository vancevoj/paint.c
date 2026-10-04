# Shared compiler settings. Every first-party target calls pc_warnings().
if(MSVC)
  set(PC_WARN_FLAGS /W4 /WX /wd4200 /wd4201 /utf-8)
else()
  set(PC_WARN_FLAGS -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Werror)
endif()

# Sanitizers apply to every target in the build, including dependencies,
# so instrumented and uninstrumented code never mix.
if(NOT MSVC)
  if(PC_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all
                        -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
  endif()
  if(PC_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
  endif()
endif()

if(WIN32)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
endif()

# pc_warnings(<target>) - strict warnings for first-party code only.
function(pc_warnings tgt)
  target_compile_options(${tgt} PRIVATE ${PC_WARN_FLAGS})
endfunction()

# pc_glob_sources(<out-var> <dir>) - all .c files directly in <dir>.
# Lanes add files without editing shared CMake files.
function(pc_glob_sources out dir)
  file(GLOB _srcs CONFIGURE_DEPENDS ${dir}/*.c)
  list(SORT _srcs)
  set(${out} ${_srcs} PARENT_SCOPE)
endfunction()
