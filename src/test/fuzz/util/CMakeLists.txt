# Copyright (c) 2023-present The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

add_library(test_fuzz STATIC EXCLUDE_FROM_ALL
  check_globals.cpp
  descriptor.cpp
  relaypool.cpp
  net.cpp
  ../fuzz.cpp
  ../util.cpp
)

target_link_libraries(test_fuzz
  PRIVATE
    core_interface
    test_util
    quicksilver_node
    Boost::headers
)

if(NOT FUZZ_BINARY_LINKS_WITHOUT_MAIN_FUNCTION)
  target_compile_definitions(test_fuzz PRIVATE PROVIDE_FUZZ_MAIN_FUNCTION)
endif()
