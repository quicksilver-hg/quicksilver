# Copyright (c) 2023-present The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

add_library(quicksilver_util STATIC EXCLUDE_FROM_ALL
  asmap.cpp
  batchpriority.cpp
  bip32.cpp
  bytevectorhash.cpp
  chaintype.cpp
  check.cpp
  exception.cpp
  workfrac.cpp
  fs.cpp
  fs_helpers.cpp
  hasher.cpp
  moneystr.cpp
  readwritefile.cpp
  serfloat.cpp
  signalinterrupt.cpp
  sock.cpp
  strencodings.cpp
  string.cpp
  syserror.cpp
  thread.cpp
  threadinterrupt.cpp
  threadnames.cpp
  time.cpp
  tokenpipe.cpp
  ../logging.cpp
  ../random.cpp
  ../randomenv.cpp
  ../streams.cpp
  ../support/lockedpool.cpp
  ../sync.cpp
)

target_link_libraries(quicksilver_util
  PRIVATE
    core_interface
    quicksilver_clientversion
    quicksilver_crypto
    $<$<PLATFORM_ID:Windows>:ws2_32>
    $<$<PLATFORM_ID:Windows>:iphlpapi>
)

# Process containment, shared by the Cuckatoo solver bridge and bundled Tor.
# Deliberately NOT part of quicksilver_util: quicksilver_cuckatoo links only
# Threads today, and folding this into the util library would drag logging,
# random, sync and lockedpool into the PoW unit, which the bench harness also
# links. This target has no dependencies beyond libc and the platform APIs --
# it does no logging and reads no configuration, by design.
add_library(quicksilver_process STATIC EXCLUDE_FROM_ALL
  process/contained_child.cpp
)

target_link_libraries(quicksilver_process
  PRIVATE
    core_interface
)
