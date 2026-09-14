# Copyright (c) 2023-present The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

include(AddWindowsResources)

configure_file(${PROJECT_SOURCE_DIR}/cmake/quicksilver-build-config.h.in quicksilver-build-config.h USE_SOURCE_PERMISSIONS @ONLY)
include_directories(${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR})

#=============================
# Subprojects
#=============================
# Subprojects include subdirectories that do or could have tests
# and/or benchmark binaries, such as all subtrees and univalue.
# These need to be included before CMAKE_*_OUTPUT_DIRECTORY variables
# are set, so output locations of subproject tests and libraries are
# not overridden.
include(../cmake/crc32c.cmake)
include(../cmake/leveldb.cmake)
add_subdirectory(univalue)
#=============================
# secp256k1 subtree
#=============================
message("")
message("Configuring secp256k1 subtree...")
set(SECP256K1_DISABLE_SHARED ON CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_ECDH OFF CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_RECOVERY ON CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_MUSIG OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_TESTS ${BUILD_TESTS} CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_EXHAUSTIVE_TESTS ${BUILD_TESTS} CACHE BOOL "" FORCE)
if(NOT BUILD_TESTS)
  # Always skip the ctime tests, if we are building no other tests.
  # Otherwise, they are built if Valgrind is available. See SECP256K1_VALGRIND.
  set(SECP256K1_BUILD_CTIME_TESTS ${BUILD_TESTS} CACHE BOOL "" FORCE)
endif()
set(SECP256K1_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
include(GetTargetInterface)
# -fsanitize and related flags apply to both C++ and C,
# so we can pass them down to libsecp256k1 as CFLAGS and LDFLAGS.
get_target_interface(SECP256K1_APPEND_CFLAGS "" sanitize_interface COMPILE_OPTIONS)
string(STRIP "${SECP256K1_APPEND_CFLAGS} ${APPEND_CPPFLAGS}" SECP256K1_APPEND_CFLAGS)
string(STRIP "${SECP256K1_APPEND_CFLAGS} ${APPEND_CFLAGS}" SECP256K1_APPEND_CFLAGS)
set(SECP256K1_APPEND_CFLAGS ${SECP256K1_APPEND_CFLAGS} CACHE STRING "" FORCE)
get_target_interface(SECP256K1_APPEND_LDFLAGS "" sanitize_interface LINK_OPTIONS)
string(STRIP "${SECP256K1_APPEND_LDFLAGS} ${APPEND_LDFLAGS}" SECP256K1_APPEND_LDFLAGS)
set(SECP256K1_APPEND_LDFLAGS ${SECP256K1_APPEND_LDFLAGS} CACHE STRING "" FORCE)
# We want to build libsecp256k1 with the most tested RelWithDebInfo configuration.
enable_language(C)
foreach(config IN LISTS CMAKE_BUILD_TYPE CMAKE_CONFIGURATION_TYPES)
  if(config STREQUAL "")
    continue()
  endif()
  string(TOUPPER "${config}" config)
  set(CMAKE_C_FLAGS_${config} "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
endforeach()
# If the CFLAGS environment variable is defined during building depends
# and configuring this build system, its content might be duplicated.
if(DEFINED ENV{CFLAGS})
  deduplicate_flags(CMAKE_C_FLAGS)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS OFF)
add_subdirectory(secp256k1)
set_target_properties(secp256k1 PROPERTIES
  EXCLUDE_FROM_ALL TRUE
)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Set top-level target output locations.
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
endif()
if(NOT CMAKE_LIBRARY_OUTPUT_DIRECTORY)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)
endif()
if(NOT CMAKE_ARCHIVE_OUTPUT_DIRECTORY)
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)
endif()

add_custom_target(generate_build_info
  BYPRODUCTS ${PROJECT_BINARY_DIR}/src/quicksilver-build-info.h
  COMMAND ${CMAKE_COMMAND} -DBUILD_INFO_HEADER_PATH=${PROJECT_BINARY_DIR}/src/quicksilver-build-info.h -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -P ${PROJECT_SOURCE_DIR}/cmake/script/GenerateBuildInfo.cmake
  COMMENT "Generating quicksilver-build-info.h"
  VERBATIM
)
add_library(quicksilver_clientversion STATIC EXCLUDE_FROM_ALL
  clientversion.cpp
)
target_link_libraries(quicksilver_clientversion
  PRIVATE
    core_interface
)
add_dependencies(quicksilver_clientversion generate_build_info)

add_subdirectory(crypto)
add_subdirectory(crypto/cuckatoo)
add_subdirectory(util)

add_library(quicksilver_consensus STATIC EXCLUDE_FROM_ALL
  arith_uint256.cpp
  consensus/merkle.cpp
  consensus/tx_check.cpp
  hash.cpp
  primitives/block.cpp
  primitives/transaction.cpp
  pubkey.cpp
  script/interpreter.cpp
  script/script.cpp
  script/script_error.cpp
  uint256.cpp
)
target_link_libraries(quicksilver_consensus
  PRIVATE
    core_interface
    quicksilver_crypto
    secp256k1
)

if(WITH_ZMQ)
  add_subdirectory(zmq)
endif()

# Home for common functionality shared by different executables and libraries.
# Similar to `quicksilver_util` library, but higher-level.
add_library(quicksilver_common STATIC EXCLUDE_FROM_ALL
  addresstype.cpp
  base58.cpp
  bech32.cpp
  chain.cpp
  chainparams.cpp
  chainparamsbase.cpp
  coins.cpp
  common/args.cpp
  common/bloom.cpp
  common/config.cpp
  common/init.cpp
  common/interfaces.cpp
  common/messages.cpp
  common/netif.cpp
  common/pcp.cpp
  common/run_command.cpp
  common/settings.cpp
  common/signmessage.cpp
  common/system.cpp
  common/url.cpp
  compressor.cpp
  core_read.cpp
  core_write.cpp
  deploymentinfo.cpp
  external_signer.cpp
  gpu/detection_service.cpp
  init/common.cpp
  kernel/chainparams.cpp
  key.cpp
  key_io.cpp
  merkleblock.cpp
  net_permissions.cpp
  net_types.cpp
  netaddress.cpp
  netbase.cpp
  outputtype.cpp
  policy/policy.cpp
  pow.cpp
  protocol.cpp
  psqt.cpp
  rpc/rawtransaction_util.cpp
  rpc/request.cpp
  rpc/util.cpp
  scheduler.cpp
  script/descriptor.cpp
  script/miniscript.cpp
  script/parsing.cpp
  script/sign.cpp
  script/signingprovider.cpp
  script/solver.cpp
)
target_link_libraries(quicksilver_common
  PRIVATE
    core_interface
    quicksilver_consensus
    quicksilver_cuckatoo
    quicksilver_util
    univalue
    secp256k1
    Boost::headers
    $<TARGET_NAME_IF_EXISTS:USDT::headers>
    $<$<PLATFORM_ID:Windows>:ws2_32>
)

add_library(quicksilver_agent STATIC EXCLUDE_FROM_ALL
  agent/agentclient.cpp
  agent/agentpeer.cpp
  agent/agentpeerset.cpp
  agent/headerdriver.cpp
  agent/headermessages.cpp
  agent/headerchain.cpp
  agent/headerpeer.cpp
  agent/headerstore.cpp
  agent/headersync.cpp
  agent/messageio.cpp
  agent/peertransport.cpp
  agent/txmessages.cpp
  agent/txpeer.cpp
  agent/allotmentpolicy.cpp
  agent/allotmentspend.cpp
  agent/interrupt.cpp
  agent/allotmentstore.cpp
)
target_link_libraries(quicksilver_agent
  PRIVATE
    core_interface
    quicksilver_common
    univalue
)

include(InstallBinaryComponent)

if(BUILD_AGENT)
  add_executable(quicksilver-agent
    quicksilver-agent.cpp
  )
  add_windows_resources(quicksilver-agent quicksilver-agent-res.rc)
  target_link_libraries(quicksilver-agent
    core_interface
    quicksilver_agent
    quicksilver_common
    quicksilver_util
    univalue
    Boost::headers
  )
  install_binary_component(quicksilver-agent)
endif()

if(ENABLE_VAULT)
  add_subdirectory(vault)

  if(BUILD_VAULT_TOOL)
    add_executable(quicksilver-vault
      quicksilver-vault.cpp
      vault/vaulttool.cpp
    )
    add_windows_resources(quicksilver-vault quicksilver-vault-res.rc)
    target_link_libraries(quicksilver-vault
      core_interface
      quicksilver_vault
      quicksilver_common
      quicksilver_util
      Boost::headers
    )
    install_binary_component(quicksilver-vault HAS_MANPAGE DEVELOPER_TOOL)
  endif()
endif()


# P2P and RPC server functionality used by `quicksilverd` and `quicksilver-qt` executables.
add_library(quicksilver_node STATIC EXCLUDE_FROM_ALL
  addrdb.cpp
  addrman.cpp
  banman.cpp
  bip324.cpp
  blockencodings.cpp
  blockfilter.cpp
  consensus/tx_verify.cpp
  dbwrapper.cpp
  deploymentstatus.cpp
  flatfile.cpp
  headerssync.cpp
  httprpc.cpp
  httpserver.cpp
  i2p.cpp
  index/base.cpp
  index/blockfilterindex.cpp
  index/coinstatsindex.cpp
  index/txindex.cpp
  init.cpp
  kernel/chain.cpp
  kernel/checks.cpp
  kernel/coinstats.cpp
  kernel/context.cpp
  kernel/cs_main.cpp
  kernel/disconnected_transactions.cpp
  kernel/relaypool_removal_reason.cpp
  mapport.cpp
  net.cpp
  net_processing.cpp
  netgroup.cpp
  node/abort.cpp
  node/blockmanager_args.cpp
  node/blockstorage.cpp
  node/caches.cpp
  node/chainstate.cpp
  node/chainstatemanager_args.cpp
  node/coin.cpp
  node/coins_view_args.cpp
  node/connection_types.cpp
  node/context.cpp
  node/database_args.cpp
  node/eviction.cpp
  node/interface_ui.cpp
  node/interfaces.cpp
  node/kernel_notifications.cpp
  node/relaypool_args.cpp
  node/relaypool_persist.cpp
  node/relaypool_persist_args.cpp
  node/miner.cpp
  node/block_solve.cpp
  node/mining_service.cpp
  node/peerman_args.cpp
  node/psqt.cpp
  node/timeoffsets.cpp
  node/transaction.cpp
  node/txdownloadman_impl.cpp
  node/warnings.cpp
  noui.cpp
  policy/packages.cpp
  policy/replacement.cpp
  policy/settings.cpp
  policy/truc_policy.cpp
  rest.cpp
  rpc/blockchain.cpp
  rpc/external_signer.cpp
  rpc/relaypool.cpp
  rpc/mining.cpp
  rpc/net.cpp
  rpc/node.cpp
  rpc/output_script.cpp
  rpc/rawtransaction.cpp
  rpc/server.cpp
  rpc/server_util.cpp
  rpc/signmessage.cpp
  rpc/txoutproof.cpp
  script/sigcache.cpp
  tor/bundled_tor.cpp
  torcontrol.cpp
  txdb.cpp
  txrelaypool.cpp
  txorphanage.cpp
  txrequest.cpp
  validation.cpp
  validationinterface.cpp
  versionbits.cpp
  $<$<TARGET_EXISTS:quicksilver_vault>:vault/init.cpp>
  $<$<NOT:$<TARGET_EXISTS:quicksilver_vault>>:dummyvault.cpp>
)
target_link_libraries(quicksilver_node
  PRIVATE
    core_interface
    quicksilver_common
    quicksilver_util
    quicksilver_process
    $<TARGET_NAME_IF_EXISTS:quicksilver_zmq>
    leveldb
    univalue
    Boost::headers
    $<TARGET_NAME_IF_EXISTS:libevent::core>
    $<TARGET_NAME_IF_EXISTS:libevent::extra>
    $<TARGET_NAME_IF_EXISTS:libevent::pthreads>
    $<TARGET_NAME_IF_EXISTS:USDT::headers>
)


# Quicksilver daemon.
if(BUILD_DAEMON)
  add_executable(quicksilverd
    quicksilverd.cpp
    init/quicksilverd.cpp
  )
  add_windows_resources(quicksilverd quicksilverd-res.rc)
  target_link_libraries(quicksilverd
    core_interface
    quicksilver_node
    $<TARGET_NAME_IF_EXISTS:quicksilver_vault>
  )
  install_binary_component(quicksilverd HAS_MANPAGE DEVELOPER_TOOL)
endif()

add_library(quicksilver_cli STATIC EXCLUDE_FROM_ALL
  compat/stdin.cpp
  rpc/client.cpp
)
target_link_libraries(quicksilver_cli
  PUBLIC
    core_interface
    univalue
)


# Quicksilver RPC client
if(BUILD_CLI)
  add_executable(quicksilver-cli quicksilver-cli.cpp)
  add_windows_resources(quicksilver-cli quicksilver-cli-res.rc)
  target_link_libraries(quicksilver-cli
    core_interface
    quicksilver_cli
    quicksilver_common
    quicksilver_util
    libevent::core
    libevent::extra
  )
  install_binary_component(quicksilver-cli HAS_MANPAGE DEVELOPER_TOOL)
endif()


if(BUILD_TX)
  add_executable(quicksilver-tx quicksilver-tx.cpp)
  add_windows_resources(quicksilver-tx quicksilver-tx-res.rc)
  target_link_libraries(quicksilver-tx
    core_interface
    quicksilver_common
    quicksilver_util
    univalue
  )
  install_binary_component(quicksilver-tx HAS_MANPAGE DEVELOPER_TOOL)
endif()


if(BUILD_UTIL)
  add_executable(quicksilver-util quicksilver-util.cpp)
  add_windows_resources(quicksilver-util quicksilver-util-res.rc)
  target_link_libraries(quicksilver-util
    core_interface
    quicksilver_common
    quicksilver_util
  )
  install_binary_component(quicksilver-util HAS_MANPAGE DEVELOPER_TOOL)
endif()


if(BUILD_GUI)
  add_subdirectory(qt)
endif()


if(BUILD_KERNEL_LIB)
  add_subdirectory(kernel)
endif()

if(BUILD_UTIL_CHAINSTATE)
  add_executable(quicksilver-chainstate
    quicksilver-chainstate.cpp
  )
  # TODO: The `SKIP_BUILD_RPATH` property setting can be deleted
  #       in the future after reordering Guix script commands to
  #       perform binary checks after the installation step.
  set_target_properties(quicksilver-chainstate PROPERTIES
    SKIP_BUILD_RPATH OFF
  )
  target_link_libraries(quicksilver-chainstate
    PRIVATE
      core_interface
      quicksilverkernel
  )
endif()


add_subdirectory(test/util)
if(BUILD_BENCH)
  add_subdirectory(bench)
endif()

if(BUILD_TESTS)
  add_subdirectory(test)
endif()

if(BUILD_FUZZ_BINARY)
  add_subdirectory(test/fuzz)
endif()
