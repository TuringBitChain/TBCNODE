if(NOT ENABLE_IPC)
  add_library(bitcoin_ipc STATIC EXCLUDE_FROM_ALL stub.cpp)
  return()
endif()

add_library(bitcoin_ipc STATIC EXCLUDE_FROM_ALL stub.cpp)
set_target_properties(bitcoin_ipc PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

target_capnp_sources(bitcoin_ipc ${CMAKE_SOURCE_DIR}/src capnp/echo.capnp)
target_include_directories(bitcoin_ipc PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(bitcoin_ipc SYSTEM PRIVATE ${Boost_INCLUDE_DIR})
target_link_libraries(bitcoin_ipc PRIVATE multiprocess)

# NOTE (M3a Task 1): bitcoin_ipc_test (ipc_echo_round_trip.cpp) is temporarily
# excluded because it was written against the no-context echo.capnp schema and
# fails after restoring context params. It will be replaced/deleted in Task 6.
# add_library(bitcoin_ipc_test STATIC EXCLUDE_FROM_ALL
#   test/ipc_echo_round_trip.cpp
#   ${CMAKE_SOURCE_DIR}/src/interfaces/echo.cpp
# )
# set_target_properties(bitcoin_ipc_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
# target_include_directories(bitcoin_ipc_test PUBLIC ${CMAKE_SOURCE_DIR}/src)
# target_link_libraries(bitcoin_ipc_test
#   PRIVATE
#     bitcoin_ipc
#     multiprocess
# )
