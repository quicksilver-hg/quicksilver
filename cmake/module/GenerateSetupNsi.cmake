# Copyright (c) 2023-present The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

function(generate_setup_nsi)
  set(abs_top_srcdir ${PROJECT_SOURCE_DIR})
  set(abs_top_builddir ${PROJECT_BINARY_DIR})
  set(CLIENT_URL ${PROJECT_HOMEPAGE_URL})
  set(CLIENT_TARNAME "quicksilver")
  set(QUICKSILVER_GUI_NAME "quicksilver-qt")
  set(QUICKSILVER_DAEMON_NAME "quicksilverd")
  set(QUICKSILVER_CLI_NAME "quicksilver-cli")
  set(QUICKSILVER_AGENT_NAME "quicksilver-agent")
  set(QUICKSILVER_TX_NAME "quicksilver-tx")
  set(QUICKSILVER_VAULT_TOOL_NAME "quicksilver-vault")
  set(QUICKSILVER_UTIL_NAME "quicksilver-util")
  set(EXEEXT ${CMAKE_EXECUTABLE_SUFFIX})
  configure_file(${PROJECT_SOURCE_DIR}/share/setup.nsi.in ${PROJECT_BINARY_DIR}/quicksilver-win64-setup.nsi USE_SOURCE_PERMISSIONS @ONLY)
endfunction()
