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
