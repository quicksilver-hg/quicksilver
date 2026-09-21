# Copyright (c) 2023-present The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

include_guard(GLOBAL)

function(setup_split_debug_script)
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    set(OBJCOPY ${CMAKE_OBJCOPY})
    set(STRIP ${CMAKE_STRIP})
    configure_file(
      contrib/devtools/split-debug.sh.in split-debug.sh
      FILE_PERMISSIONS OWNER_READ OWNER_EXECUTE
                       GROUP_READ GROUP_EXECUTE
                       WORLD_READ
      @ONLY
    )
  endif()
endfunction()

function(add_maintenance_targets)
  if(NOT PYTHON_COMMAND)
    return()
  endif()

  foreach(target IN ITEMS quicksilverd quicksilver-qt quicksilver-cli quicksilver-agent quicksilver-tx quicksilver-util quicksilver-vault test_quicksilver bench_quicksilver)
    if(TARGET ${target})
      list(APPEND executables $<TARGET_FILE:${target}>)
    endif()
  endforeach()

  add_custom_target(check-symbols
    COMMAND ${CMAKE_COMMAND} -E echo "Running symbol and dynamic library checks..."
    COMMAND ${PYTHON_COMMAND} ${PROJECT_SOURCE_DIR}/contrib/devtools/symbol-check.py ${executables}
    VERBATIM
  )

  add_custom_target(check-security
    COMMAND ${CMAKE_COMMAND} -E echo "Checking binary security..."
    COMMAND ${PYTHON_COMMAND} ${PROJECT_SOURCE_DIR}/contrib/devtools/security-check.py ${executables}
    VERBATIM
  )
endfunction()

function(add_windows_deploy_target)
  # Every binary that share/setup.nsi.in bundles. Leaving the target undefined
  # when one of them is missing is deliberate: an installer cannot be built out
  # of a configuration that does not produce what it installs.
  set(deploy_binaries
    quicksilver-qt
    quicksilverd
    quicksilver-cli
    quicksilver-agent
    quicksilver-tx
    quicksilver-vault
    quicksilver-util
  )

  # `--target deploy` against a configuration that never defined it fails with
  # "unknown target", which reads like operator error rather than a missing
  # prerequisite. Wherever this function declines to define the target, it says
  # so here, once, with the reason, so the configure log can answer the question.
  if(NOT WIN32)
    message(STATUS "Windows installer: `deploy` target not defined, because the target system is ${CMAKE_SYSTEM_NAME}, not Windows.")
    return()
  endif()

  set(missing_binaries "")
  foreach(target IN LISTS deploy_binaries)
    if(NOT TARGET ${target})
      list(APPEND missing_binaries ${target})
    endif()
  endforeach()
  if(missing_binaries)
    list(JOIN missing_binaries " " missing_report)
    message(STATUS "Windows installer: `deploy` target not defined, because the installer bundles binaries this configuration does not build: ${missing_report}.")
    return()
  endif()

  # TODO: Consider replacing this code with the CPack NSIS Generator.
  #       See https://cmake.org/cmake/help/latest/cpack_gen/nsis.html
  # Generated either way, so that the configured script is available to run by
  # hand once NSIS is installed.
  include(GenerateSetupNsi)
  generate_setup_nsi()

  # makensis is a host tool rather than a property of the toolchain, and a
  # default NSIS install leaves it off PATH. CMake retries a find_program whose
  # cache entry is NOTFOUND, so installing NSIS and reconfiguring picks it up.
  find_program(MAKENSIS_EXECUTABLE
    NAMES makensis
    PATHS "$ENV{ProgramFiles}/NSIS" "$ENV{ProgramFiles\(x86\)}/NSIS"
    DOC "Path to the NSIS makensis compiler, used to build the Windows installer."
  )
  if(NOT MAKENSIS_EXECUTABLE)
    # Deliberately still defined. This is a Windows build of every binary the
    # installer ships, so `deploy` is a reasonable thing to ask for, and the one
    # prerequisite left is a host tool the operator can install without touching
    # the build. Answering that with "unknown target" is the silence this exists
    # to remove -- so the target exists and fails on its first command, naming
    # what is missing, before staging anything.
    message(STATUS "Windows installer: `deploy` target defined, but it will fail, because makensis was not found. Install NSIS 3.x and re-run CMake.")
    add_custom_target(deploy
      COMMAND ${CMAKE_COMMAND} -E echo "Cannot build the Windows installer: makensis was not found when this build directory was configured."
      COMMAND ${CMAKE_COMMAND} -E echo "Install NSIS 3.x from https://nsis.sourceforge.io, then re-run CMake so that makensis is located."
      COMMAND ${CMAKE_COMMAND} -E false
      VERBATIM
    )
    return()
  endif()

  # setup.nsi.in takes all seven binaries out of release/, so whatever stages
  # them has to populate that directory; only the staging step differs by
  # toolchain. Each binary is named on its own line rather than looped over,
  # because test/lint/lint-desktop-packaging.py reads these genexes to check
  # what the installer ships.
  if(MINGW)
    # strip is doing two jobs here: removing the debug symbols the GNU toolchain
    # leaves in the binary, and being the thing that copies it into release/.
    set(stage_commands
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:quicksilver-qt> -o ${PROJECT_BINARY_DIR}/release/$<TARGET_FILE_NAME:quicksilver-qt>
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:quicksilverd> -o ${PROJECT_BINARY_DIR}/release/$<TARGET_FILE_NAME:quicksilverd>
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:quicksilver-cli> -o ${PROJECT_BINARY_DIR}/release/$<TARGET_FILE_NAME:quicksilver-cli>
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:quicksilver-agent> -o ${PROJECT_BINARY_DIR}/release/$<TARGET_FILE_NAME:quicksilver-agent>
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:quicksilver-tx> -o ${PROJECT_BINARY_DIR}/release/$<TARGET_FILE_NAME:quicksilver-tx>
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:quicksilver-vault> -o ${PROJECT_BINARY_DIR}/release/$<TARGET_FILE_NAME:quicksilver-vault>
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:quicksilver-util> -o ${PROJECT_BINARY_DIR}/release/$<TARGET_FILE_NAME:quicksilver-util>
    )
  else()
    # MSVC keeps debug info in a separate .pdb, so a Release .exe has nothing to
    # strip -- and CMAKE_STRIP is a binutils tool that MSVC does not set at all,
    # which would leave the command line starting with its own -o. The MSVC
    # counterpart of the strip above is therefore a plain copy.
    set(stage_commands
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:quicksilver-qt> ${PROJECT_BINARY_DIR}/release/$<TARGET_FILE_NAME:quicksilver-qt>
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:quicksilverd> ${PROJECT_BINARY_DIR}/release/$<TARGET_FILE_NAME:quicksilverd>
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:quicksilver-cli> ${PROJECT_BINARY_DIR}/release/$<TARGET_FILE_NAME:quicksilver-cli>
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:quicksilver-agent> ${PROJECT_BINARY_DIR}/release/$<TARGET_FILE_NAME:quicksilver-agent>
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:quicksilver-tx> ${PROJECT_BINARY_DIR}/release/$<TARGET_FILE_NAME:quicksilver-tx>
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:quicksilver-vault> ${PROJECT_BINARY_DIR}/release/$<TARGET_FILE_NAME:quicksilver-vault>
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:quicksilver-util> ${PROJECT_BINARY_DIR}/release/$<TARGET_FILE_NAME:quicksilver-util>
    )
  endif()

  add_custom_command(
    OUTPUT ${PROJECT_BINARY_DIR}/quicksilver-win64-setup.exe
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PROJECT_BINARY_DIR}/release
    ${stage_commands}
    COMMAND ${MAKENSIS_EXECUTABLE} -V2 ${PROJECT_BINARY_DIR}/quicksilver-win64-setup.nsi
    VERBATIM
  )
  add_custom_target(deploy DEPENDS ${PROJECT_BINARY_DIR}/quicksilver-win64-setup.exe)
endfunction()

function(add_macos_deploy_target)
  if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND TARGET quicksilver-qt)
    set(macos_app "Quicksilver-Qt.app")
    # Populate Contents subdirectory.
    configure_file(${PROJECT_SOURCE_DIR}/share/qt/Info.plist.in ${macos_app}/Contents/Info.plist NO_SOURCE_PERMISSIONS)
    file(CONFIGURE OUTPUT ${macos_app}/Contents/PkgInfo CONTENT "APPL????")
    # Populate Contents/Resources subdirectory.
    file(CONFIGURE OUTPUT ${macos_app}/Contents/Resources/empty.lproj CONTENT "")
    configure_file(${PROJECT_SOURCE_DIR}/src/qt/res/icons/quicksilver.icns ${macos_app}/Contents/Resources/quicksilver.icns NO_SOURCE_PERMISSIONS COPYONLY)
    file(CONFIGURE OUTPUT ${macos_app}/Contents/Resources/Base.lproj/InfoPlist.strings
      CONTENT "{ CFBundleDisplayName = \"@CLIENT_NAME@\"; CFBundleName = \"@CLIENT_NAME@\"; }"
    )

    add_custom_command(
      OUTPUT ${PROJECT_BINARY_DIR}/${macos_app}/Contents/MacOS/Quicksilver-Qt
      COMMAND ${CMAKE_COMMAND} --install ${PROJECT_BINARY_DIR} --config $<CONFIG> --component quicksilver-qt --prefix ${macos_app}/Contents/MacOS --strip
      COMMAND ${CMAKE_COMMAND} -E rename ${macos_app}/Contents/MacOS/bin/$<TARGET_FILE_NAME:quicksilver-qt> ${macos_app}/Contents/MacOS/Quicksilver-Qt
      COMMAND ${CMAKE_COMMAND} -E rm -rf ${macos_app}/Contents/MacOS/bin
      COMMAND ${CMAKE_COMMAND} -E rm -rf ${macos_app}/Contents/MacOS/share
      VERBATIM
    )

    add_custom_target(deploydir
      DEPENDS ${PROJECT_BINARY_DIR}/${macos_app}/Contents/MacOS/Quicksilver-Qt
    )
    add_custom_target(deploy
      DEPENDS deploydir
    )
    add_dependencies(deploydir quicksilver-qt)
  endif()
endfunction()
