if(NOT ENABLE_IPC)
  add_library(bitcoin_ipc STATIC EXCLUDE_FROM_ALL stub.cpp)
  return()
endif()

add_library(bitcoin_ipc STATIC EXCLUDE_FROM_ALL stub.cpp)
set_target_properties(bitcoin_ipc PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

target_capnp_sources(bitcoin_ipc ${CMAKE_SOURCE_DIR}/src
  capnp/echo.capnp
  capnp/common.capnp
)
target_include_directories(bitcoin_ipc PUBLIC ${CMAKE_SOURCE_DIR}/src)
# bitcoin_ipc includes TBC headers (serialize.h, streams.h, primitives/) that
# require HAVE_CONFIG_H + the build-generated config so that compat/endian.h
# and compat/byteswap.h skip re-declarations of system-provided functions.
target_include_directories(bitcoin_ipc PRIVATE ${CMAKE_BINARY_DIR}/src)
target_compile_definitions(bitcoin_ipc PRIVATE HAVE_CONFIG_H)
target_include_directories(bitcoin_ipc SYSTEM PRIVATE ${Boost_INCLUDE_DIR})
target_link_libraries(bitcoin_ipc
  PRIVATE
    multiprocess
    univalue
)

# bitcoin_ipc_test: C++20 static lib for the CTransaction serialization
# round-trip test (Task 2 of M3a).  A thin C++17 Boost runner in
# ipc_tests.cpp calls SerializeRoundTripTest() from test_bitcoin.
add_library(bitcoin_ipc_test STATIC EXCLUDE_FROM_ALL
  test/ipc_test.cpp
)
set_target_properties(bitcoin_ipc_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_capnp_sources(bitcoin_ipc_test ${CMAKE_SOURCE_DIR}/src
  test/ipc_test.capnp
)
target_include_directories(bitcoin_ipc_test PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(bitcoin_ipc_test PRIVATE ${CMAKE_BINARY_DIR}/src)
target_compile_definitions(bitcoin_ipc_test PRIVATE HAVE_CONFIG_H)
target_include_directories(bitcoin_ipc_test SYSTEM PRIVATE ${Boost_INCLUDE_DIR})
target_link_libraries(bitcoin_ipc_test
  PRIVATE
    bitcoin_ipc
    multiprocess
    univalue
)
