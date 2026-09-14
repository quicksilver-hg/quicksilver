# Copyright (c) 2023-present The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  enable_language(OBJCXX)
  set(CMAKE_OBJCXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
  set(CMAKE_OBJCXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
  set(CMAKE_OBJCXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
  set(CMAKE_OBJCXX_FLAGS_MINSIZEREL "${CMAKE_CXX_FLAGS_MINSIZEREL}")
  string(APPEND CMAKE_OBJCXX_COMPILE_OBJECT " ${APPEND_CPPFLAGS} ${APPEND_CXXFLAGS}")
endif()

get_target_property(qt_lib_type Qt5::Core TYPE)

function(import_plugins target)
  if(qt_lib_type STREQUAL "STATIC_LIBRARY")
    set(plugins Qt5::QMinimalIntegrationPlugin)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      list(APPEND plugins Qt5::QXcbIntegrationPlugin)
    elseif(WIN32)
      list(APPEND plugins Qt5::QWindowsIntegrationPlugin Qt5::QWindowsVistaStylePlugin)
    elseif(APPLE)
      list(APPEND plugins Qt5::QCocoaIntegrationPlugin Qt5::QMacStylePlugin)
    endif()
    qt5_import_plugins(${target}
      INCLUDE ${plugins}
      EXCLUDE_BY_TYPE imageformats iconengines
    )
  endif()
endfunction()

# For Qt-specific commands and variables, please consult:
#  - https://cmake.org/cmake/help/latest/manual/cmake-qt.7.html
#  - https://doc.qt.io/qt-5/cmake-manual.html

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOMOC_MOC_OPTIONS "-p${CMAKE_CURRENT_SOURCE_DIR}")
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOUIC_SEARCH_PATHS forms)

add_library(quicksilver_qt STATIC EXCLUDE_FROM_ALL
  bantablemodel.cpp
  bantablemodel.h
  quicksilver.cpp
  quicksilver.h
  quicksilveraddressvalidator.cpp
  quicksilveraddressvalidator.h
  quicksilveramountfield.cpp
  quicksilveramountfield.h
  quicksilvergui.cpp
  quicksilvergui.h
  quicksilverunits.cpp
  quicksilverunits.h
  clientmodel.cpp
  clientmodel.h
  csvmodelwriter.cpp
  csvmodelwriter.h
  guiutil.cpp
  maturity.cpp
  guiutil.h
  initexecutor.cpp
  initexecutor.h
  intro.cpp
  intro.h
  $<$<PLATFORM_ID:Darwin>:macdockiconhandler.h>
  $<$<PLATFORM_ID:Darwin>:macdockiconhandler.mm>
  $<$<PLATFORM_ID:Darwin>:macnotificationhandler.h>
  $<$<PLATFORM_ID:Darwin>:macnotificationhandler.mm>
  $<$<PLATFORM_ID:Darwin>:macos_appnap.h>
  $<$<PLATFORM_ID:Darwin>:macos_appnap.mm>
  modaloverlay.cpp
  modaloverlay.h
  networkstyle.cpp
  networkstyle.h
  notificator.cpp
  notificator.h
  optionsdialog.cpp
  optionsdialog.h
  optionsmodel.cpp
  optionsmodel.h
  peertablemodel.cpp
  peertablemodel.h
  peertablesortproxy.cpp
  peertablesortproxy.h
  platformstyle.cpp
  platformstyle.h
  quicksilverstyle.cpp
  quicksilverstyle.h
  qvalidatedlineedit.cpp
  qvalidatedlineedit.h
  qvaluecombobox.cpp
  qvaluecombobox.h
  rpcconsole.cpp
  rpcconsole.h
  splashscreen.cpp
  splashscreen.h
  trafficgraphwidget.cpp
  trafficgraphwidget.h
  utilitydialog.cpp
  utilitydialog.h
  $<$<PLATFORM_ID:Windows>:winshutdownmonitor.cpp>
  $<$<PLATFORM_ID:Windows>:winshutdownmonitor.h>
  quicksilver.qrc
)
target_compile_definitions(quicksilver_qt
  PUBLIC
    QT_NO_KEYWORDS
    QT_USE_QSTRINGBUILDER
)
target_include_directories(quicksilver_qt
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)
set_property(SOURCE macnotificationhandler.mm
  # Ignore warnings "'NSUserNotificationCenter' is deprecated: first deprecated in macOS 11.0".
  APPEND PROPERTY COMPILE_OPTIONS -Wno-deprecated-declarations
)
target_link_libraries(quicksilver_qt
  PUBLIC
    Qt5::Widgets
  PRIVATE
    core_interface
    quicksilver_cli
    leveldb
    univalue
    Boost::headers
    $<TARGET_NAME_IF_EXISTS:QRencode::QRencode>
    $<$<PLATFORM_ID:Darwin>:-framework\ AppKit>
    $<$<CXX_COMPILER_ID:MSVC>:shlwapi>
)

if(ENABLE_VAULT)
  target_sources(quicksilver_qt
    PRIVATE
      addressbookpage.cpp
      addressbookpage.h
      agentallotmentpage.cpp
      agentallotmentpage.h
      addresstablemodel.cpp
      addresstablemodel.h
      askpassphrasedialog.cpp
      askpassphrasedialog.h
      coincontroldialog.cpp
      coincontroldialog.h
      coincontroltreewidget.cpp
      coincontroltreewidget.h
      consensusreviewpage.cpp
      consensusreviewpage.h
      createvaultdialog.cpp
      createvaultdialog.h
      desktoplaunchpage.cpp
      desktoplaunchpage.h
      editaddressdialog.cpp
      editaddressdialog.h
      minemintpage.cpp
      minemintpage.h
      miningmodel.cpp
      miningmodel.h
      networkpage.cpp
      networkpage.h
      openuridialog.cpp
      openuridialog.h
      overviewpage.cpp
      overviewpage.h
      pagestack.cpp
      pagestack.h
      paymentserver.cpp
      paymentserver.h
      psqtoperationsdialog.cpp
      psqtoperationsdialog.h
      qrimagewidget.cpp
      qrimagewidget.h
      receivecoinsdialog.cpp
      receivecoinsdialog.h
      receiverequestdialog.cpp
      receiverequestdialog.h
      recentrequeststablemodel.cpp
      recentrequeststablemodel.h
      sendcoinsdialog.cpp
      sendcoinsdialog.h
      sendcoinsentry.cpp
      sendcoinsentry.h
      signverifymessagedialog.cpp
      signverifymessagedialog.h
      transactiondesc.cpp
      transactiondesc.h
      transactiondescdialog.cpp
      transactiondescdialog.h
      transactionfilterproxy.cpp
      transactionfilterproxy.h
      transactionoverviewwidget.cpp
      transactionoverviewwidget.h
      transactionrecord.cpp
      transactionrecord.h
      transactiontablemodel.cpp
      transactiontablemodel.h
      transactionview.cpp
      transactionview.h
      thinvaultheadersource.cpp
      thinvaultheadersource.h
      vaultcontroller.cpp
      vaultcontroller.h
      vaultframe.cpp
      vaultframe.h
      vaultmodel.cpp
      vaultmodel.h
      vaultmodeltransaction.cpp
      vaultmodeltransaction.h
      vaultsummary.cpp
      vaultsummary.h
      vaultview.cpp
      vaultview.h
  )
  target_link_libraries(quicksilver_qt
    PRIVATE
      quicksilver_vault
      quicksilver_agent
      Qt5::Network
  )
endif()

if(WITH_DBUS)
  target_link_libraries(quicksilver_qt PRIVATE Qt5::DBus)
endif()

if(qt_lib_type STREQUAL "STATIC_LIBRARY")
  # We want to define static plugins to link ourselves, thus preventing
  # automatic linking against a "sane" set of default static plugins.
  qt5_import_plugins(quicksilver_qt
      EXCLUDE_BY_TYPE bearer iconengines imageformats platforms styles
  )
endif()

add_executable(quicksilver-qt
  main.cpp
  ../init/quicksilver-qt.cpp
)

add_windows_resources(quicksilver-qt res/quicksilver-qt-res.rc)

target_link_libraries(quicksilver-qt
  core_interface
  quicksilver_qt
  quicksilver_node
)

import_plugins(quicksilver-qt)
install_binary_component(quicksilver-qt HAS_MANPAGE)

# Linux/BSD desktop integration (matches the DBus guard used elsewhere in the
# build). macOS ships a .app bundle and Windows an NSIS installer, so both are
# excluded here. GNUInstallDirs is already in scope (used by the manpage install
# above). These install rules are the single source of truth for the desktop
# entry, AppStream metainfo, and menu icons; the Debian packaging and the
# AppImage build both consume them rather than copying these files again.
if(NOT CMAKE_SYSTEM_NAME MATCHES "(Windows|Darwin)")
  install(FILES ${PROJECT_SOURCE_DIR}/share/applications/io.github.quicksilver_hg.quicksilver_qt.desktop
          DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  install(FILES ${PROJECT_SOURCE_DIR}/share/applications/io.github.quicksilver_hg.quicksilver_qt.metainfo.xml
          DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
  foreach(icon_size IN ITEMS 16 32 64 128 256)
    install(FILES ${PROJECT_SOURCE_DIR}/share/pixmaps/quicksilver${icon_size}.png
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${icon_size}x${icon_size}/apps
            RENAME quicksilver.png)
  endforeach()
  # Legacy /usr/share/pixmaps fallback for desktop environments that don't
  # consult the hicolor theme. It must resolve under the .desktop Icon key
  # (Icon=quicksilver), so the 128px XPM is renamed to the unqualified name.
  install(FILES ${PROJECT_SOURCE_DIR}/share/pixmaps/quicksilver128.xpm
          DESTINATION ${CMAKE_INSTALL_DATADIR}/pixmaps
          RENAME quicksilver.xpm)
endif()
if(WIN32)
  set_target_properties(quicksilver-qt PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

if(BUILD_GUI_TESTS)
  add_subdirectory(test)
endif()
