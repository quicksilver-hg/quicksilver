# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# F-334. Fail if the GUI executable has a COPY relocation against a Qt symbol.
# That relocation means this binary owns a copy of Qt data (staticMetaObject,
# QCoreApplication::self, ...) which libQt5Gui does not use. The platform
# plugin then crashes in QGuiApplication::screenAdded. readelf needs no display.
if(NOT DEFINED BINARY)
  message(FATAL_ERROR "F-334: BINARY is not set")
endif()
if(NOT EXISTS "${BINARY}")
  message(FATAL_ERROR "F-334: binary does not exist: ${BINARY}")
endif()
find_program(READELF_EXECUTABLE readelf)
if(NOT READELF_EXECUTABLE)
  message(FATAL_ERROR "F-334: readelf not found")
endif()
execute_process(
  COMMAND "${READELF_EXECUTABLE}" -rW "${BINARY}"
  OUTPUT_VARIABLE relocs
  ERROR_VARIABLE relocs_err
  RESULT_VARIABLE relocs_rc
)
if(NOT relocs_rc EQUAL 0)
  message(FATAL_ERROR "F-334: readelf -rW failed (${relocs_rc}): ${relocs_err}")
endif()
set(hits "")
string(REPLACE "\n" ";" lines "${relocs}")
foreach(line IN LISTS lines)
  if(line MATCHES "_COPY" AND line MATCHES "Qt_")
    string(APPEND hits "${line}\n")
  endif()
endforeach()
if(NOT hits STREQUAL "")
  message(FATAL_ERROR "F-334: COPY relocation against a Qt symbol:\n${hits}")
endif()
