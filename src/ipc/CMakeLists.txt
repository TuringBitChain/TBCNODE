# Real IPC sources are added in a later task, gated by ENABLE_IPC in src/CMakeLists.txt.
add_library(bitcoin_ipc STATIC EXCLUDE_FROM_ALL stub.cpp)
