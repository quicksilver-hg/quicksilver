# Copyright (c) 2024-present The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

include(GNUInstallDirs)

# TODO: libquicksilverkernel is a work in progress consensus engine
#       library, as more and more modules are decoupled from the
#       consensus engine, this list will shrink to only those
#       which are absolutely necessary.
add_library(quicksilverkernel
  quicksilverkernel.cpp
  chain.cpp
  checks.cpp
  chainparams.cpp
  coinstats.cpp
  context.cpp
  cs_main.cpp
  disconnected_transactions.cpp
  relaypool_removal_reason.cpp
  ../arith_uint256.cpp
  ../chain.cpp
  ../coins.cpp
  ../compressor.cpp
  ../consensus/merkle.cpp
  ../consensus/tx_check.cpp
  ../consensus/tx_verify.cpp
  ../core_read.cpp
  ../dbwrapper.cpp
  ../deploymentinfo.cpp
  ../deploymentstatus.cpp
  ../flatfile.cpp
  ../hash.cpp
  ../logging.cpp
  ../node/blockstorage.cpp
  ../node/chainstate.cpp
  ../policy/packages.cpp
  ../policy/policy.cpp
  ../policy/replacement.cpp
  ../policy/settings.cpp
  ../policy/truc_policy.cpp
  ../pow.cpp
  ../primitives/block.cpp
  ../primitives/transaction.cpp
  ../pubkey.cpp
  ../random.cpp
  ../randomenv.cpp
  ../script/interpreter.cpp
  ../script/script.cpp
  ../script/script_error.cpp
  ../script/sigcache.cpp
  ../script/solver.cpp
  ../streams.cpp
  ../support/lockedpool.cpp
  ../sync.cpp
  ../txdb.cpp
  ../txrelaypool.cpp
  ../uint256.cpp
  ../util/chaintype.cpp
  ../util/check.cpp
  ../util/workfrac.cpp
  ../util/fs.cpp
  ../util/fs_helpers.cpp
  ../util/hasher.cpp
  ../util/moneystr.cpp
  ../util/serfloat.cpp
  ../util/signalinterrupt.cpp
  ../util/strencodings.cpp
  ../util/string.cpp
  ../util/syserror.cpp
  ../util/exception.cpp
  ../util/thread.cpp
  ../util/threadnames.cpp
  ../util/time.cpp
  ../util/tokenpipe.cpp
  ../validation.cpp
  ../validationinterface.cpp
  ../versionbits.cpp
)
target_link_libraries(quicksilverkernel
  PRIVATE
    core_interface
    quicksilver_clientversion
    quicksilver_crypto
    quicksilver_cuckatoo
    leveldb
    secp256k1
    $<TARGET_NAME_IF_EXISTS:USDT::headers>
  PUBLIC
    Boost::headers
)

# libquicksilverkernel requires default symbol visibility, explicitly
# specify that here so that things still work even when user
# configures with -DREDUCE_EXPORTS=ON
#
# Note this is a quick hack that will be removed as we
# incrementally define what to export from the library.
set_target_properties(quicksilverkernel PROPERTIES
  CXX_VISIBILITY_PRESET default
)

# Add a convenience libquicksilverkernel target as a synonym for quicksilverkernel.
add_custom_target(libquicksilverkernel)
add_dependencies(libquicksilverkernel quicksilverkernel)

# When building the static library, install all static libraries the
# quicksilverkernel depends on.
if(NOT BUILD_SHARED_LIBS)
  # Recursively get all the static libraries a target depends on and put them in libs_out
  function(get_target_static_link_libs target libs_out)
    get_target_property(linked_libraries ${target} LINK_LIBRARIES)
    foreach(dep ${linked_libraries})
      if(TARGET ${dep})
        add_dependencies(libquicksilverkernel ${dep})
        get_target_property(dep_type ${dep} TYPE)
        if(dep_type STREQUAL "STATIC_LIBRARY")
          list(APPEND ${libs_out} ${dep})
          get_target_static_link_libs(${dep} ${libs_out})
        endif()
      endif()
    endforeach()
    set(${libs_out} ${${libs_out}} PARENT_SCOPE)
  endfunction()

  set(all_kernel_static_link_libs "")
  get_target_static_link_libs(quicksilverkernel all_kernel_static_link_libs)

  install(TARGETS ${all_kernel_static_link_libs} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT libquicksilverkernel)
  list(TRANSFORM all_kernel_static_link_libs PREPEND "-l")
  # LIBS_PRIVATE is substituted in the pkg-config file.
  list(JOIN all_kernel_static_link_libs " " LIBS_PRIVATE)
endif()

configure_file(${PROJECT_SOURCE_DIR}/libquicksilverkernel.pc.in ${PROJECT_BINARY_DIR}/libquicksilverkernel.pc @ONLY)
install(FILES ${PROJECT_BINARY_DIR}/libquicksilverkernel.pc DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig" COMPONENT libquicksilverkernel)

install(TARGETS quicksilverkernel
  RUNTIME
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT libquicksilverkernel
  LIBRARY
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT libquicksilverkernel
  ARCHIVE
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT libquicksilverkernel
)
