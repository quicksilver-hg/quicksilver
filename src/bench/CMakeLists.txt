# Copyright (c) 2023-present The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

add_executable(bench_quicksilver
  bench_quicksilver.cpp
  bench.cpp
  nanobench.cpp
# Benchmarks:
  addrman.cpp
  base58.cpp
  bech32.cpp
  bip324_ecdh.cpp
  block_assemble.cpp
  ccoins_caching.cpp
  chacha20.cpp
  checkblock.cpp
  checkblockindex.cpp
  checkqueue.cpp
  cluster_linearize.cpp
  crypto_hash.cpp
  descriptors.cpp
  disconnected_transactions.cpp
  duplicate_inputs.cpp
  ellswift.cpp
  examples.cpp
  gcs_filter.cpp
  hashpadding.cpp
  index_blockfilter.cpp
  load_external.cpp
  lockedpool.cpp
  logging.cpp
  relaypool_eviction.cpp
  relaypool_stress.cpp
  merkle_root.cpp
  parse_hex.cpp
  peer_eviction.cpp
  poly1305.cpp
  pool.cpp
  prevector.cpp
  random.cpp
  readwriteblock.cpp
  rollingbloom.cpp
  rpc_blockchain.cpp
  rpc_relaypool.cpp
  sign_transaction.cpp
  streams_findbyte.cpp
  strencodings.cpp
  util_time.cpp
  verify_script.cpp
  xor.cpp
)

include(TargetDataSources)
target_raw_data_sources(bench_quicksilver NAMESPACE benchmark::data
  data/quicksilver_block.raw
)

target_link_libraries(bench_quicksilver
  core_interface
  test_util
  quicksilver_node
  Boost::headers
)

if(ENABLE_VAULT)
  target_sources(bench_quicksilver
    PRIVATE
      coin_selection.cpp
      vault_balance.cpp
      vault_create.cpp
      vault_create_tx.cpp
      vault_loading.cpp
      vault_ismine.cpp
  )
  target_link_libraries(bench_quicksilver quicksilver_vault)
endif()

add_test(NAME bench_sanity_check_high_priority
  COMMAND bench_quicksilver -sanity-check -priority-level=high
)

install_binary_component(bench_quicksilver DEVELOPER_TOOL)
