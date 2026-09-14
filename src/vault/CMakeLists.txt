# Copyright (c) 2023-present The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# Vault functionality used by quicksilverd and quicksilver-vault executables.
add_library(quicksilver_vault STATIC EXCLUDE_FROM_ALL
  coincontrol.cpp
  coinselection.cpp
  context.cpp
  archive.cpp
  crypter.cpp
  db.cpp
  dump.cpp
  external_signer_scriptpubkeyman.cpp
  interfaces.cpp
  load.cpp
  receive.cpp
  rpc/addresses.cpp
  rpc/backup.cpp
  rpc/coins.cpp
  rpc/encrypt.cpp
  rpc/signmessage.cpp
  rpc/spend.cpp
  rpc/transactions.cpp
  rpc/util.cpp
  rpc/vault.cpp
  scriptpubkeyman.cpp
  spend.cpp
  transaction.cpp
  vault.cpp
  vaultdb.cpp
  vaultutil.cpp
)
target_link_libraries(quicksilver_vault
  PRIVATE
    core_interface
    quicksilver_agent
    quicksilver_common
    univalue
    Boost::headers
    $<TARGET_NAME_IF_EXISTS:USDT::headers>
)

if(NOT USE_SQLITE)
  message(FATAL_ERROR "Vault functionality requested but no SQLite support available.")
endif()
if(USE_SQLITE)
  target_sources(quicksilver_vault PRIVATE sqlite.cpp)
  target_link_libraries(quicksilver_vault
    PRIVATE
      $<TARGET_NAME_IF_EXISTS:unofficial::sqlite3::sqlite3>
      $<TARGET_NAME_IF_EXISTS:SQLite::SQLite3>
  )
endif()
