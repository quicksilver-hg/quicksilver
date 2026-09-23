# Copyright (c) 2025-present The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

include_guard(GLOBAL)
include(GNUInstallDirs)

# Mark the optional command-line tools DEVELOPER_TOOL. The desktop application
# provisions consensus itself; headless operators use the daemon and RPC client.
# These components are still built and tested when QS_DEVELOPER_TOOLS is OFF;
# only their install() rules are skipped. Debian sets the option ON and splits
# the staged tree into packages with .install files.
function(install_binary_component component)
  cmake_parse_arguments(PARSE_ARGV 1
    IC                              # prefix
    "HAS_MANPAGE;DEVELOPER_TOOL"    # options
    ""                              # one_value_keywords
    ""                              # multi_value_keywords
  )
  if(IC_DEVELOPER_TOOL AND NOT QS_DEVELOPER_TOOLS)
    return()
  endif()
  set(target_name ${component})
  install(TARGETS ${target_name}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT ${component}
  )
  if(INSTALL_MAN AND IC_HAS_MANPAGE)
    install(FILES ${PROJECT_SOURCE_DIR}/doc/man/${target_name}.1
      DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
      COMPONENT ${component}
    )
  endif()
endfunction()
