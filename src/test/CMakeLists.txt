# Copyright (c) 2023-present The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# Do not use generator expressions in test sources because the
# SOURCES property is processed to gather test suite macros.
add_executable(test_quicksilver
  main.cpp
  addrman_tests.cpp
  allocator_tests.cpp
  amount_tests.cpp
  agent_headerchain_tests.cpp
  argsman_tests.cpp
  arith_uint256_tests.cpp
  banman_tests.cpp
  base32_tests.cpp
  base58_tests.cpp
  base64_tests.cpp
  bech32_tests.cpp
  bip32_tests.cpp
  bip324_tests.cpp
  blockchain_tests.cpp
  blockencodings_tests.cpp
  blockfilter_index_tests.cpp
  blockfilter_tests.cpp
  blockmanager_tests.cpp
  bloom_tests.cpp
  bundled_tor_tests.cpp
  bswap_tests.cpp
  checkqueue_tests.cpp
  chaintype_tests.cpp
  cluster_linearize_tests.cpp
  coins_tests.cpp
  coinscachepair_tests.cpp
  coinstatsindex_tests.cpp
  common_url_tests.cpp
  compilerbug_tests.cpp
  compress_tests.cpp
  contained_child_tests.cpp
  crypto_tests.cpp
  cuckatoo_bench_gpu_csv_tests.cpp
  cuckatoo_bench_registry_tests.cpp
  cuckatoo_bench_solve_tests.cpp
  cuckatoo_tests.cpp
  cuckatoo_verify_codes_tests.cpp
  cuckoocache_tests.cpp
  gpu_parity_tests.cpp
  gpu_detection_tests.cpp
  gpu_solver_tests.cpp
  dbwrapper_tests.cpp
  denialofservice_tests.cpp
  descriptor_tests.cpp
  disconnected_transactions.cpp
  flatfile_tests.cpp
  fs_tests.cpp
  getarg_tests.cpp
  hash_tests.cpp
  headers_sync_chainwork_tests.cpp
  httpserver_tests.cpp
  i2p_tests.cpp
  interfaces_tests.cpp
  key_io_tests.cpp
  key_tests.cpp
  logging_tests.cpp
  relaypool_tests.cpp
  merkle_tests.cpp
  merkleblock_tests.cpp
  miner_tests.cpp
  mining_anchor_tests.cpp
  miniscript_tests.cpp
  multisig_tests.cpp
  net_peer_connection_tests.cpp
  net_peer_eviction_tests.cpp
  net_tests.cpp
  netbase_tests.cpp
  node_warnings_tests.cpp
  orphanage_tests.cpp
  outputtype_tests.cpp
  pcp_tests.cpp
  peerman_tests.cpp
  pmt_tests.cpp
  pool_tests.cpp
  pow_tests.cpp
  prevector_tests.cpp
  raii_event_tests.cpp
  random_tests.cpp
  replacement_tests.cpp
  rest_tests.cpp
  result_tests.cpp
  reverselock_tests.cpp
  rpc_tests.cpp
  sanity_tests.cpp
  scheduler_tests.cpp
  script_p2sh_tests.cpp
  script_parse_tests.cpp
  script_segwit_tests.cpp
  script_standard_tests.cpp
  script_tests.cpp
  scriptnum_tests.cpp
  serfloat_tests.cpp
  serialize_tests.cpp
  settings_tests.cpp
  sighash_tests.cpp
  sigopcount_tests.cpp
  skiplist_tests.cpp
  sock_tests.cpp
  span_tests.cpp
  streams_tests.cpp
  sync_tests.cpp
  system_tests.cpp
  timeoffsets_tests.cpp
  torcontrol_tests.cpp
  transaction_tests.cpp
  translation_tests.cpp
  txdownload_tests.cpp
  txindex_tests.cpp
  txpackage_tests.cpp
  txpow_tests.cpp
  txwork_ranking_tests.cpp
  mint_tests.cpp
  mining_service_tests.cpp
  txrequest_tests.cpp
  txvalidation_tests.cpp
  txvalidationcache_tests.cpp
  uint256_tests.cpp
  util_string_tests.cpp
  util_tests.cpp
  util_threadnames_tests.cpp
  util_trace_tests.cpp
  validation_block_tests.cpp
  validation_chainstate_tests.cpp
  validation_chainstatemanager_tests.cpp
  validation_flush_tests.cpp
  validation_tests.cpp
  validationinterface_tests.cpp
  versionbits_tests.cpp
)

include(TargetDataSources)
target_json_data_sources(test_quicksilver
  data/base58_encode_decode.json
  data/bip341_vault_vectors.json
  data/blockfilters.json
  data/key_io_invalid.json
  data/key_io_valid.json
  data/script_tests.json
  data/sighash.json
  data/tx_invalid.json
  data/tx_valid.json
)
target_raw_data_sources(test_quicksilver NAMESPACE test::data
  data/asmap.raw
)

target_link_libraries(test_quicksilver
  core_interface
  test_util
  quicksilver_agent
  quicksilver_cli
  quicksilver_node
  quicksilver_consensus
  quicksilver_cuckatoo
  quicksilver_cuckatoo_bench
  quicksilver_process
  secp256k1
  Boost::headers
  libevent::extra
)

# A real second process for UtilTests/test_LockDirectory. The datadir lock's
# promise is that ANOTHER PROCESS cannot take it, which one process cannot check
# about itself; upstream forked, which is why the check was #ifndef WIN32 and so
# never ran on the platform where a stale lock is hardest to see. A compiled
# executable rather than a script because CreateProcess cannot launch a .bat.
add_executable(test_lock_dir_stub lockdirstub.cpp)
target_link_libraries(test_lock_dir_stub PRIVATE core_interface quicksilver_util)
add_dependencies(test_quicksilver test_lock_dir_stub)

# A scripted stand-in for the GPU solver. The bridge spawns the solver with a
# real argv and no shell, so a fixture has to be something the OS can execute
# directly -- see solverstub.cpp.
add_executable(test_solver_stub solverstub.cpp)
target_link_libraries(test_solver_stub PRIVATE core_interface)
add_dependencies(test_quicksilver test_solver_stub)

# A scripted stand-in for tor. Like the solver stub, a compiled executable and
# not a script: the supervisor spawns with a real argv and no shell, and
# CreateProcess cannot launch a .bat. It reads the torrc we generate, so the
# generated file is part of what the tests exercise.
add_executable(test_tor_stub torstub.cpp)
target_link_libraries(test_tor_stub PRIVATE core_interface)
add_dependencies(test_quicksilver test_tor_stub)

# Hand the tests each stub's exact path. Deriving it from argv[0] guesses at both
# the directory layout and the platform's executable suffix; the generator
# expression is what the build system already knows.
target_compile_definitions(test_quicksilver PRIVATE
  LOCK_DIR_STUB_PATH="$<TARGET_FILE:test_lock_dir_stub>"
  SOLVER_STUB_PATH="$<TARGET_FILE:test_solver_stub>"
  TOR_STUB_PATH="$<TARGET_FILE:test_tor_stub>"
)

if(ENABLE_VAULT)
  add_subdirectory(${PROJECT_SOURCE_DIR}/src/vault/test vault)
endif()

function(add_boost_test source_file)
  if(NOT EXISTS ${source_file})
    return()
  endif()

  file(READ "${source_file}" source_file_content)
  string(REGEX
    MATCH "(BOOST_FIXTURE_TEST_SUITE|BOOST_AUTO_TEST_SUITE)\\(([A-Za-z0-9_]+)"
    test_suite_macro "${source_file_content}"
  )
  string(REGEX
    REPLACE "(BOOST_FIXTURE_TEST_SUITE|BOOST_AUTO_TEST_SUITE)\\(" ""
    test_suite_name "${test_suite_macro}"
  )
  if(test_suite_name)
    add_test(NAME ${test_suite_name}
      COMMAND test_quicksilver --run_test=${test_suite_name} --catch_system_error=no --log_level=test_suite -- DEBUG_LOG_OUT
    )
    set_property(TEST ${test_suite_name} PROPERTY
      SKIP_REGULAR_EXPRESSION "no test cases matching filter"
    )
  endif()
endfunction()

function(add_all_test_targets)
  get_target_property(test_source_dir test_quicksilver SOURCE_DIR)
  get_target_property(test_sources test_quicksilver SOURCES)
  foreach(test_source ${test_sources})
    cmake_path(IS_RELATIVE test_source result)
    if(result)
      cmake_path(APPEND test_source_dir ${test_source} OUTPUT_VARIABLE test_source)
    endif()
    add_boost_test(${test_source})
  endforeach()
endfunction()

add_all_test_targets()

# F-142: gpu_parity_tests skips when CUCKATOO_GPU_SOLVER is unset. Boost
# reports those cases as skipped, but ctest still records the suite as
# Passed unless this suite's skip regex matches the skip reason. Do not
# add this string globally -- unrelated suites that happen to log it
# would start skipping.
set_property(TEST gpu_parity_tests APPEND PROPERTY
  SKIP_REGULAR_EXPRESSION "CUCKATOO_GPU_SOLVER unset"
)

install_binary_component(test_quicksilver DEVELOPER_TOOL)
