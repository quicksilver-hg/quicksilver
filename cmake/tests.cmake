# Copyright (c) 2023-present The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

if(TARGET quicksilver-util AND TARGET quicksilver-tx AND PYTHON_COMMAND)
  add_test(NAME util_test_runner
    COMMAND ${CMAKE_COMMAND} -E env QUICKSILVERUTIL=$<TARGET_FILE:quicksilver-util> QUICKSILVERTX=$<TARGET_FILE:quicksilver-tx> ${PYTHON_COMMAND} ${PROJECT_BINARY_DIR}/test/util/test_runner.py
  )
endif()

if(PYTHON_COMMAND)
  add_test(NAME util_rpcauth_test
    COMMAND ${PYTHON_COMMAND} ${PROJECT_BINARY_DIR}/test/util/rpcauth-test.py
  )
endif()
