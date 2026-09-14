# Copyright (c) 2024-present The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

set(CMAKE_AUTOMOC_MOC_OPTIONS "-p${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(test_quicksilver-qt
  apptests.cpp
  guiutiltests.cpp
  introtests.cpp
  maturitytests.cpp
  optiontests.cpp
  quicksilverstyletests.cpp
  rpcnestedtests.cpp
  storagecoststests.cpp
  test_main.cpp
  uritests.cpp
  util.cpp
  ../../init/quicksilver-qt.cpp
)

target_link_libraries(test_quicksilver-qt
  core_interface
  quicksilver_qt
  test_util
  quicksilver_node
  Boost::headers
  Qt5::Test
)

import_plugins(test_quicksilver-qt)

if(ENABLE_VAULT)
  target_sources(test_quicksilver-qt
    PRIVATE
      addressbooktests.cpp
      vaultsummarytests.cpp
      vaulttests.cpp
      ../../vault/test/vault_test_fixture.cpp
  )
endif()

# A real executable the GPU-solver startup probe can actually start, for
# OptionTests::gpuSolverSettingPersistsAndRequiresRestart. See
# solverprobestub.cpp for why a script fixture cannot serve on Windows.
add_executable(test_solver_probe_stub solverprobestub.cpp)
target_link_libraries(test_solver_probe_stub PRIVATE core_interface)
# Nothing here is a QObject; skip the moc pass this directory turns on by default.
set_target_properties(test_solver_probe_stub PROPERTIES AUTOMOC OFF AUTOUIC OFF)
add_dependencies(test_quicksilver-qt test_solver_probe_stub)

add_test(NAME test_quicksilver-qt
  COMMAND test_quicksilver-qt
)
if(WIN32 AND VCPKG_TARGET_TRIPLET)
  # On Windows, vcpkg configures Qt with `-opengl dynamic`, which makes
  # the "minimal" platform plugin unusable due to internal Qt bugs.
  set_tests_properties(test_quicksilver-qt PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=windows"
  )
endif()

install_binary_component(test_quicksilver-qt DEVELOPER_TOOL)
