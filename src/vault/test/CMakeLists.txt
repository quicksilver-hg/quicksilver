# Copyright (c) 2023-present The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# Do not use generator expressions in test sources because the
# SOURCES property is processed to gather test suite macros.
target_sources(test_quicksilver
  PRIVATE
    init_test_fixture.cpp
    vault_test_fixture.cpp
    db_tests.cpp
    coinselector_tests.cpp
    group_outputs_tests.cpp
    init_tests.cpp
    psqt_vault_tests.cpp
    scriptpubkeyman_tests.cpp
    spend_tests.cpp
    vault_crypto_tests.cpp
    vault_tests.cpp
    vault_transaction_tests.cpp
    vaultload_tests.cpp
)
target_link_libraries(test_quicksilver quicksilver_vault)
