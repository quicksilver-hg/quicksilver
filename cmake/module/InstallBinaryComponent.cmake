# Copyright (c) 2025-present The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

include_guard(GLOBAL)
include(GNUInstallDirs)

# Mark a component DEVELOPER_TOOL when it is a command-line tool a person using
# Quicksilver never needs. Such a component is still built and still tested --
# only its install() rules are skipped when QS_DEVELOPER_TOOLS is OFF, which is
# how a release package ends up placing one application on a user's system
# rather than eleven executables they have to choose between.
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
