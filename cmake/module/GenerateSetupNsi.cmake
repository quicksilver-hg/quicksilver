# Copyright (c) 2023-present The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

function(generate_setup_nsi)
  # makensis resolves these itself when it compiles the script, and on Windows
  # its File and Icon commands do not accept forward slashes -- makensis reports
  # "no files found" for a path that exists, and mixing separators fails the same
  # way, so the whole path has to be native. TO_NATIVE_PATH gives backslashes on
  # Windows and leaves a POSIX path alone, which is what a MinGW cross-build from
  # Linux needs. The directory variables keep a trailing separator so the script
  # can append a configured binary name to them.
  file(TO_NATIVE_PATH "${PROJECT_BINARY_DIR}/release/" nsis_release_dir)
  file(TO_NATIVE_PATH "${PROJECT_SOURCE_DIR}/share/rpcauth/" nsis_rpcauth_dir)
  file(TO_NATIVE_PATH "${PROJECT_SOURCE_DIR}/share/pixmaps/quicksilver.ico" nsis_app_icon)
  file(TO_NATIVE_PATH "${PROJECT_SOURCE_DIR}/share/pixmaps/nsis-wizard.bmp" nsis_wizard_bitmap)
  file(TO_NATIVE_PATH "${PROJECT_SOURCE_DIR}/share/pixmaps/nsis-header.bmp" nsis_header_bitmap)
  file(TO_NATIVE_PATH "${PROJECT_SOURCE_DIR}/COPYING" nsis_copying)
  file(TO_NATIVE_PATH "${PROJECT_SOURCE_DIR}/doc/README_windows.txt" nsis_readme_windows)
  file(TO_NATIVE_PATH "${PROJECT_SOURCE_DIR}/share/examples/quicksilver.conf" nsis_example_conf)
  file(TO_NATIVE_PATH "${PROJECT_SOURCE_DIR}/doc/developer-tools.md" nsis_developer_tools_doc)
  set(CLIENT_URL ${PROJECT_HOMEPAGE_URL})
  set(CLIENT_TARNAME "quicksilver")
  set(QUICKSILVER_GUI_NAME "quicksilver")
  set(QUICKSILVER_DAEMON_NAME "quicksilver-daemon")
  set(QUICKSILVER_CLI_NAME "quicksilver-cli")
  set(QUICKSILVER_AGENT_NAME "quicksilver-agent")
  set(QUICKSILVER_TX_NAME "quicksilver-tx")
  set(QUICKSILVER_VAULT_TOOL_NAME "quicksilver-vault")
  set(EXEEXT ${CMAKE_EXECUTABLE_SUFFIX})
  configure_file(${PROJECT_SOURCE_DIR}/share/setup.nsi.in ${PROJECT_BINARY_DIR}/quicksilver-win64-setup.nsi USE_SOURCE_PERMISSIONS @ONLY)
endfunction()
