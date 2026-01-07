cmake_minimum_required(VERSION 3.5)

include(ExternalProject)

# -----------------------------------------------------------------------------
# Tools
# -----------------------------------------------------------------------------
find_program(SECP256K1_UP_OBJCOPY NAMES llvm-objcopy objcopy)
find_program(SECP256K1_UP_NM      NAMES llvm-nm nm)

if(NOT SECP256K1_UP_OBJCOPY OR NOT SECP256K1_UP_NM)
  message(FATAL_ERROR "objcopy/nm not found (required for upstream secp256k1 symbol prefixing)")
endif()

# -----------------------------------------------------------------------------
# Paths
# -----------------------------------------------------------------------------
set(UP_SRC   ${CMAKE_SOURCE_DIR}/third_party/secp256k1_upstream)
set(UP_BUILD ${CMAKE_BINARY_DIR}/secp256k1_up_build)

# upstream build output archive (NO install step)
set(UP_LIB_RAW  ${UP_BUILD}/lib/libsecp256k1.a)

# prefixed archive (output)
set(UP_LIB_PREF ${CMAKE_BINARY_DIR}/libsecp256k1_up.a)

# symbol map file
set(UP_SYM_MAP  ${UP_BUILD}/secp256k1_upstream_symbols.map)

# -----------------------------------------------------------------------------
# 1) Build upstream secp256k1 (no install)
# -----------------------------------------------------------------------------
ExternalProject_Add(secp256k1_up_ext
  SOURCE_DIR  ${UP_SRC}
  BINARY_DIR  ${UP_BUILD}
  CMAKE_ARGS
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DBUILD_SHARED_LIBS=OFF
    -DSECP256K1_INSTALL=OFF
    -DSECP256K1_BUILD_TESTS=OFF
    -DSECP256K1_BUILD_BENCHMARK=OFF
    -DSECP256K1_BUILD_EXHAUSTIVE_TESTS=OFF
    -DSECP256K1_BUILD_CTIME_TESTS=OFF
    -DSECP256K1_BUILD_EXAMPLES=OFF
    -DSECP256K1_ENABLE_MODULE_SCHNORRSIG=ON
    -DSECP256K1_ENABLE_MODULE_EXTRAKEYS=ON
    -DSECP256K1_ENABLE_MODULE_ECDH=OFF
    -DSECP256K1_ENABLE_MODULE_RECOVERY=OFF
    -DSECP256K1_ENABLE_MODULE_MUSIG=OFF
    -DSECP256K1_ENABLE_MODULE_ELLSWIFT=OFF
  INSTALL_COMMAND ""
  BUILD_BYPRODUCTS ${UP_LIB_RAW}
)

# -----------------------------------------------------------------------------
# 2) Prefix ONLY upstream-defined secp256k1_* symbols (safe)
#    Avoid objcopy --prefix-symbols (it renames libc/stack protector refs too)
# -----------------------------------------------------------------------------
set(UP_PREFIX_SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/prefix_upstream_secp256k1.cmake)

file(WRITE ${UP_PREFIX_SCRIPT} [==[
cmake_minimum_required(VERSION 3.5)

if(NOT DEFINED NM OR NOT DEFINED OBJCOPY OR NOT DEFINED IN_LIB OR NOT DEFINED OUT_LIB OR NOT DEFINED MAP_FILE)
  message(FATAL_ERROR "prefix script: missing -DNM/-DOBJCOPY/-DIN_LIB/-DOUT_LIB/-DMAP_FILE")
endif()

execute_process(
  COMMAND "${NM}" -g --defined-only "${IN_LIB}"
  OUTPUT_VARIABLE NM_OUT
  ERROR_VARIABLE  NM_ERR
  RESULT_VARIABLE NM_RC
)

if(NOT NM_RC EQUAL 0)
  message(FATAL_ERROR "nm failed: ${NM_RC}\n${NM_ERR}")
endif()

string(REPLACE "\r\n" "\n" NM_OUT "${NM_OUT}")
string(REPLACE "\r"   "\n" NM_OUT "${NM_OUT}")
string(REPLACE "\n" ";" NM_LINES "${NM_OUT}")

set(MAP_CONTENT "")
foreach(L IN LISTS NM_LINES)
  if(L MATCHES ":$")
    continue()
  endif()

  # Take the last token as the symbol name
  if(L MATCHES ".*[ \t]([A-Za-z0-9_]+)$")
    set(SYM "${CMAKE_MATCH_1}")
    if(SYM MATCHES "^secp256k1_")
      string(REPLACE "secp256k1_" "secp256k1_up_" NEW_SYM "${SYM}")
      string(APPEND MAP_CONTENT "${SYM} ${NEW_SYM}\n")
    endif()
  endif()
endforeach()

if(MAP_CONTENT STREQUAL "")
  message(FATAL_ERROR "symbol map is empty; nm output unexpected?")
endif()

file(WRITE "${MAP_FILE}" "${MAP_CONTENT}")

execute_process(
  COMMAND "${OBJCOPY}" "--redefine-syms=${MAP_FILE}" "${IN_LIB}" "${OUT_LIB}"
  ERROR_VARIABLE  OBJCOPY_ERR
  RESULT_VARIABLE OBJCOPY_RC
)

if(NOT OBJCOPY_RC EQUAL 0)
  message(FATAL_ERROR "objcopy failed: ${OBJCOPY_RC}\n${OBJCOPY_ERR}")
endif()
]==])

add_custom_command(
  OUTPUT ${UP_LIB_PREF}
  COMMAND ${CMAKE_COMMAND}
          -DNM=${SECP256K1_UP_NM}
          -DOBJCOPY=${SECP256K1_UP_OBJCOPY}
          -DIN_LIB=${UP_LIB_RAW}
          -DOUT_LIB=${UP_LIB_PREF}
          -DMAP_FILE=${UP_SYM_MAP}
          -P ${UP_PREFIX_SCRIPT}
  DEPENDS secp256k1_up_ext
  COMMENT "Prefixing upstream secp256k1 symbols -> secp256k1_up_*"
  VERBATIM
)

add_custom_target(secp256k1_up_pref ALL DEPENDS ${UP_LIB_PREF})

# -----------------------------------------------------------------------------
# 3) IMPORTED library target for the prefixed archive
# -----------------------------------------------------------------------------
add_library(secp256k1_up STATIC IMPORTED GLOBAL)
set_target_properties(secp256k1_up PROPERTIES
  IMPORTED_LOCATION ${UP_LIB_PREF}
)
add_dependencies(secp256k1_up secp256k1_up_pref)

# -----------------------------------------------------------------------------
# 4) Your wrapper library
# -----------------------------------------------------------------------------
add_library(with_schnorr_secp256k1
  schnorr_wrap.c
)

target_include_directories(with_schnorr_secp256k1 PRIVATE ${UP_SRC}/include)
target_link_libraries(with_schnorr_secp256k1 PRIVATE secp256k1_up)

# Ensure this wrapper library doesn't expose upstream secp256k1 headers to other targets
# Only our wrapper functions should be used externally
target_include_directories(with_schnorr_secp256k1 INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)