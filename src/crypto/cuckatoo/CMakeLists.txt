# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# Quicksilver Cuckatoo PoW unit: runtime-EDGEBITS verify + lean CPU solver,
# vendored verbatim from Tromp's reference (third_party/cuckoo).
#
# The per-EDGEBITS verify/solve translation units (verify_19.cpp, solve_19.cpp, ...)
# are added by later plan tasks. The vendored cuckatoo.h has no include guard and
# defines file-scope symbols by design, so each EDGEBITS instantiation lives in its
# own translation unit wrapped in a distinct namespace — never included twice per TU.
add_library(quicksilver_cuckatoo STATIC EXCLUDE_FROM_ALL
  proofhash.cpp
  keys.cpp
  verify_19.cpp
  verify_28.cpp
  solve_19.cpp
  solve_28.cpp
  dispatch.cpp
  gpu_solver.cpp
)

# Vendored C, kept in its own target on purpose (F-271). warn_interface's flags
# come from try_append_cxx_flags, which probes them with check_cxx_source_compiles
# but attaches them to every language -- so a C translation unit collects
# -Woverloaded-virtual and -Wsuggest-override and cc1 warns about the flags
# themselves. blake2b-ref.c is the only .c source in any first-party target, so
# isolating it is cheaper than changing how every target gets its flags. The
# objects land in quicksilver_cuckatoo's archive exactly as before; the vendored
# setheader() in each EDGEBITS translation unit references them by name, so
# archive-member selection still pulls them in.
add_library(quicksilver_cuckatoo_blake2 OBJECT EXCLUDE_FROM_ALL
  vendor/blake2b-ref.c
)

target_include_directories(quicksilver_cuckatoo
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
  PUBLIC
    ${PROJECT_SOURCE_DIR}/src
)

# The lean solver uses pthreads (run_solver spawns worker threads).
find_package(Threads REQUIRED)
# warn_interface only: WERROR (-Werror / /WX) hangs off core_interface (F-268).
target_link_libraries(quicksilver_cuckatoo
  PUBLIC Threads::Threads quicksilver_process
  PRIVATE warn_interface quicksilver_cuckatoo_blake2
)

# Calibration-only single-graph harness (Stage 1 of the tx-cost flag day).
add_subdirectory(bench)
