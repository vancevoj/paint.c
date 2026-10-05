# Lane F: effect plugin fixtures for tests/app/test_f_plugins.c (one valid
# sample, a second valid plugin in a subfolder and seven malformed ones,
# plus a file that is not a library). Each fixture is a MODULE named like a
# real plugin (no "lib" prefix, pal_lib_suffix()) built into
#   <build>/tests/plugins/fixtures/good/            valid plugins
#   <build>/tests/plugins/fixtures/good/nested/     one level down
#   <build>/tests/plugins/fixtures/bad/             malformed plugins
# The generator expression keeps multi-config generators from adding a
# per-configuration subfolder.
set(_fxp_out ${CMAKE_CURRENT_BINARY_DIR}/fixtures)
if(APPLE)
  set(_fxp_suffix .dylib)
elseif(WIN32)
  set(_fxp_suffix .dll)
else()
  set(_fxp_suffix .so)
endif()
set(_fxp_targets)

function(pc_fx_fixture name src subdir)
  add_library(${name} MODULE ${src})
  target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/include)
  set_target_properties(${name} PROPERTIES PREFIX "" SUFFIX ${_fxp_suffix}
                        LIBRARY_OUTPUT_DIRECTORY "${_fxp_out}/${subdir}$<0:>"
                        RUNTIME_OUTPUT_DIRECTORY "${_fxp_out}/${subdir}$<0:>")
  if(NOT MSVC)
    target_link_libraries(${name} PRIVATE m)
  endif()
  pc_warnings(${name})
  set(_fxp_targets ${_fxp_targets} ${name} PARENT_SCOPE)
endfunction()

pc_fx_fixture(fxp_sample sample_plugin.c good)
pc_fx_fixture(fxp_swap sample_swap.c good/nested)
foreach(k 1 2 3 4 5 6 7)
  pc_fx_fixture(fxp_bad${k} bad_plugins.c bad)
  target_compile_definitions(fxp_bad${k} PRIVATE BAD_KIND=${k})
endforeach()
file(WRITE ${_fxp_out}/bad/garbage${_fxp_suffix} "this is not a shared library\n")

if(TARGET test_f_plugins)
  add_dependencies(test_f_plugins ${_fxp_targets})
  target_compile_definitions(test_f_plugins PRIVATE AFX_FIXTURES="${_fxp_out}"
                             AFX_SUFFIX="${_fxp_suffix}")
endif()
