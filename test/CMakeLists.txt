# Copyright (c) 2023-present The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

function(create_test_config)
  set(abs_top_srcdir ${PROJECT_SOURCE_DIR})
  set(abs_top_builddir ${PROJECT_BINARY_DIR})
  set(EXEEXT ${CMAKE_EXECUTABLE_SUFFIX})

  macro(set_configure_variable var conf_var)
    if(${var})
      set(${conf_var}_TRUE "")
    else()
      set(${conf_var}_TRUE "#")
    endif()
  endmacro()

  set_configure_variable(ENABLE_VAULT ENABLE_VAULT)
  set_configure_variable(WITH_SQLITE USE_SQLITE)
  set_configure_variable(BUILD_CLI BUILD_QUICKSILVER_CLI)
  set_configure_variable(BUILD_VAULT_TOOL BUILD_QUICKSILVER_VAULT)
  set_configure_variable(BUILD_AGENT BUILD_QUICKSILVER_AGENT)
  set_configure_variable(BUILD_DAEMON BUILD_QUICKSILVERDAEMON)
  set_configure_variable(BUILD_FUZZ_BINARY ENABLE_FUZZ_BINARY)
  set_configure_variable(WITH_ZMQ ENABLE_ZMQ)
  set_configure_variable(ENABLE_EXTERNAL_SIGNER ENABLE_EXTERNAL_SIGNER)
  set_configure_variable(WITH_USDT ENABLE_USDT_TRACEPOINTS)

  configure_file(config.ini.in config.ini USE_SOURCE_PERMISSIONS @ONLY)
endfunction()

create_test_config()

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/functional)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fuzz)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/util)

# The functional suite is launched out of the build tree, so this mirror is the
# code that actually runs. __pycache__ is untracked residue of the source tree;
# linking it in points every build directory at one shared bytecode cache.
file(GLOB_RECURSE functional_tests RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} functional/*)
list(FILTER functional_tests EXCLUDE REGEX "(^|/)__pycache__/")

set(test_scripts ${functional_tests} fuzz/test_runner.py util/rpcauth-test.py util/test_runner.py)

# Re-run this glob when a test is added or removed, rather than leaving the new
# test absent from the build tree until an unrelated edit forces a reconfigure.
# Watching the directories rather than passing CONFIGURE_DEPENDS to the glob keeps
# bytecode churn out of it: a __pycache__ write moves that directory's timestamp,
# not its parent's, and the glob above would otherwise have recorded every .pyc.
set(watched_dirs ${CMAKE_CURRENT_SOURCE_DIR}/functional)
foreach(script IN LISTS functional_tests)
  cmake_path(GET script PARENT_PATH script_dir)
  if(script_dir)
    list(APPEND watched_dirs ${CMAKE_CURRENT_SOURCE_DIR}/${script_dir})
  endif()
endforeach()
list(REMOVE_DUPLICATES watched_dirs)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${watched_dirs})

if(CMAKE_HOST_WIN32)
  set(symlink)
else()
  set(symlink SYMBOLIC)
endif()

set(mirrored_paths)
foreach(script IN LISTS test_scripts)
  set(source_path ${CMAKE_CURRENT_SOURCE_DIR}/${script})
  set(binary_path ${CMAKE_CURRENT_BINARY_DIR}/${script})
  list(APPEND mirrored_paths ${binary_path})

  # file(CREATE_LINK) will not create the destination's parent directory, so the
  # first entry the glob visited in each subdirectory failed and fell through
  # COPY_ON_ERROR to a real copy. That copy then never tracked the source again.
  cmake_path(GET binary_path PARENT_PATH binary_dir)
  file(MAKE_DIRECTORY ${binary_dir})

  # Replace whatever is already at the destination unless it is exactly the link
  # we are about to create: stale copies left by the fallback above, and links
  # aimed at a source tree this build directory is no longer configured against.
  if(IS_SYMLINK ${binary_path})
    file(READ_SYMLINK ${binary_path} current_target)
    if(NOT current_target STREQUAL source_path)
      file(REMOVE ${binary_path})
    endif()
  elseif(EXISTS ${binary_path})
    file(REMOVE ${binary_path})
  endif()

  file(CREATE_LINK ${source_path} ${binary_path} COPY_ON_ERROR ${symlink})
endforeach()

# Drop entries the glob no longer produces. A deleted test used to linger as a
# dangling symlink, and a deleted file that the fallback above had copied lingered
# as a live, readable file -- test/functional/data/README.md outlived its deletion
# that way. Bytecode a local test run wrote into the build tree is left alone; a
# symlink into the source tree's cache is not.
file(GLOB_RECURSE mirror_present ${CMAKE_CURRENT_BINARY_DIR}/functional/*)
foreach(present IN LISTS mirror_present)
  if(present IN_LIST mirrored_paths)
    continue()
  endif()
  if(present MATCHES "(^|/)__pycache__/" AND NOT IS_SYMLINK ${present})
    continue()
  endif()
  file(REMOVE ${present})
endforeach()

unset(functional_tests)
unset(test_scripts)
unset(mirrored_paths)
unset(mirror_present)
unset(watched_dirs)
