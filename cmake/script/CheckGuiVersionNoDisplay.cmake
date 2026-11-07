# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# F-597. On Linux -version and -help only print, so they must work on a machine
# with no display: exit 0 with the text on stdout, and no datadir created.
if(NOT DEFINED BINARY)
  message(FATAL_ERROR "F-597: BINARY is not set")
endif()
if(NOT EXISTS "${BINARY}")
  message(FATAL_ERROR "F-597: binary does not exist: ${BINARY}")
endif()
if(NOT DEFINED WORK_DIR)
  message(FATAL_ERROR "F-597: WORK_DIR is not set")
endif()

# Nothing the run reads or writes may be the user's own profile.
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(ENV{HOME} "${WORK_DIR}")
set(ENV{XDG_CONFIG_HOME} "${WORK_DIR}/config")
set(ENV{XDG_DATA_HOME} "${WORK_DIR}/data")
unset(ENV{WAYLAND_DISPLAY})
unset(ENV{QT_QPA_PLATFORM})
set(datadir "${WORK_DIR}/never-created")

function(check_print label option expected)
  # A timeout, because a window waiting on a display nobody can see would hang.
  execute_process(
    COMMAND "${BINARY}" "${option}" "-datadir=${datadir}"
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    RESULT_VARIABLE rc
    TIMEOUT 120
  )
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "F-597: ${option} with ${label} did not exit 0 (${rc}):\n${err}")
  endif()
  if(NOT out MATCHES "${expected}")
    message(FATAL_ERROR "F-597: ${option} with ${label} printed no '${expected}' on stdout:\n${out}")
  endif()
  # The platform that makes this work has no application fonts, and a version
  # query has no use for any: Qt's complaint about loading them is not output.
  if(err MATCHES "application fonts")
    message(FATAL_ERROR "F-597: ${option} with ${label} tried to load fonts:\n${err}")
  endif()
  if(EXISTS "${datadir}")
    message(FATAL_ERROR "F-597: ${option} with ${label} created the datadir")
  endif()
endfunction()

unset(ENV{DISPLAY})
check_print("no display" -version "^Quicksilver version v")
check_print("no display" -help "^Quicksilver version v.*Usage: quicksilver ")

# Set but dead, as a stale ssh session leaves it.
set(ENV{DISPLAY} ":4095")
check_print("a dead DISPLAY" -version "^Quicksilver version v")
