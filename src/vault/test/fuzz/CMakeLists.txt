# Copyright (c) 2023-present The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

target_sources(fuzz
  PRIVATE
    coincontrol.cpp
    coinselection.cpp
    crypter.cpp
    $<$<BOOL:${USE_SQLITE}>:${CMAKE_CURRENT_LIST_DIR}/notifications.cpp>
    $<$<BOOL:${USE_SQLITE}>:${CMAKE_CURRENT_LIST_DIR}/scriptpubkeyman.cpp>
    spend.cpp
)
target_link_libraries(fuzz quicksilver_vault)
