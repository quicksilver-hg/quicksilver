# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# Usage: cmake -DTOR_EXE=<path to tor.exe> -DEXPECTED_SHA256=<hex>
#              [-DSTAGE_TO=<path>] -P cmake/script/CheckBundledTor.cmake
#
# The Windows installer ships tor.exe, so the bytes it ships are checked here,
# when the installer is built, not only when contrib/tor/fetch-tor.ps1 unpacked
# them: a file replaced after configure must not reach an installer. On a match
# the file is copied to STAGE_TO, which is the only place setup.nsi takes it from.
cmake_minimum_required(VERSION 3.22)
if(NOT DEFINED TOR_EXE OR NOT DEFINED EXPECTED_SHA256)
  message(FATAL_ERROR "bundled tor: TOR_EXE and EXPECTED_SHA256 are required")
endif()
if(NOT EXISTS "${TOR_EXE}")
  message(FATAL_ERROR "bundled tor: ${TOR_EXE} does not exist. Run "
    "contrib\\tor\\fetch-tor.ps1, or set QUICKSILVER_BUNDLED_TOR_EXE to its tor.exe.")
endif()
file(SHA256 "${TOR_EXE}" actual)
string(TOLOWER "${EXPECTED_SHA256}" expected)
if(NOT actual STREQUAL expected)
  message(FATAL_ERROR "bundled tor: SHA-256 mismatch for ${TOR_EXE}\n"
    "  expected ${expected}\n  actual   ${actual}\n"
    "The installer was not built. Re-run contrib\\tor\\fetch-tor.ps1; if the pinned "
    "bundle moved, change the pins there and in cmake/module/Maintenance.cmake together.")
endif()
message(STATUS "bundled tor: SHA-256 verified ${actual}")
if(DEFINED STAGE_TO)
  file(COPY_FILE "${TOR_EXE}" "${STAGE_TO}" ONLY_IF_DIFFERENT)
endif()
