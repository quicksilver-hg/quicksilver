# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
file(READ "${IN}" content)
string(FIND "${content}" ")QSEMBED\"" collision)
if(NOT collision EQUAL -1)
  message(FATAL_ERROR "${IN} contains the raw-string delimiter )QSEMBED\"")
endif()
file(WRITE "${OUT}.tmp" "// Generated from ${IN}. Do not edit.\n#pragma once\nstatic const char ${SYMBOL}[] = R\"QSEMBED(${content})QSEMBED\";\n")
file(COPY_FILE "${OUT}.tmp" "${OUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUT}.tmp")
