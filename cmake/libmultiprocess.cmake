# Adds the vendored libmultiprocess subtree as a build dependency. Adapted from Bitcoin Core's
# cmake/libmultiprocess.cmake; the upstream `core_interface` propagation is omitted (no such
# target in TBCNODE — the subtree builds with its own defaults under our C++17/hidden-visibility).
function(add_libmultiprocess subdir)
  set(BUILD_TESTING ${BUILD_TESTS})
  add_subdirectory(${subdir} EXCLUDE_FROM_ALL)
  foreach(target multiprocess mputil mpgen)
    if(TARGET ${target})
      mark_as_advanced(${target})
    endif()
  endforeach()
  # Keep capnp's imported-location cache vars out of the main cache UI.
  get_cmake_property(_vars CACHE_VARIABLES)
  foreach(_v ${_vars})
    if(_v MATCHES "^CapnProto_.*_IMPORTED_LOCATION")
      mark_as_advanced(${_v})
    endif()
  endforeach()
endfunction()
