# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# Calibration-only Cuckatoo harness. EXCLUDE_FROM_ALL and never added to
# quicksilver_cuckatoo — nothing here may reach a node binary.
#
# OBJECT, not STATIC: the generated per-size translation units register
# themselves from static initialisers, and their symbols are never referenced by
# name. A linker discards such objects from a static archive; an object library
# contributes every object to its consumers unconditionally.
# One translation unit per EDGEBITS: the vendored cuckatoo.h has no include
# guard and defines file-scope symbols, so instantiations cannot share a TU.
#
# 19 is not part of the sweep. It is built because both this harness and the
# CONSENSUS verifier exist at that size, so it is where the harness gets checked
# against the code the network actually runs. Since the E28 flag day that is also
# true of 28, which is the stronger check of the two -- it is the size the public
# networks run. 29 stays in the sweep as the pre-flag-day baseline the E28
# measurements are quoted against.
set(QS_BENCH_EDGEBITS 19 22 23 24 25 26 27 28 29)
set(QS_BENCH_GENERATED "")
foreach(QS_EDGEBITS IN LISTS QS_BENCH_EDGEBITS)
  set(_gen "${CMAKE_CURRENT_BINARY_DIR}/solve_one_e${QS_EDGEBITS}.cpp")
  configure_file(solve_one.cpp.in "${_gen}" @ONLY)
  list(APPEND QS_BENCH_GENERATED "${_gen}")
endforeach()

add_library(quicksilver_cuckatoo_bench OBJECT EXCLUDE_FROM_ALL
  registry.cpp
  gpu_csv.cpp
  ${QS_BENCH_GENERATED}
)

target_include_directories(quicksilver_cuckatoo_bench
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
  PUBLIC
    ${PROJECT_SOURCE_DIR}/src
)

find_package(Threads REQUIRED)
target_link_libraries(quicksilver_cuckatoo_bench PUBLIC Threads::Threads)

# The calibration CLI. EXCLUDE_FROM_ALL, but it lands in the same bin/ directory
# as the node binaries (CMAKE_RUNTIME_OUTPUT_DIRECTORY, set in src/CMakeLists.txt)
# so the functional test framework can find it where it looks for everything else.
add_executable(qscalibrate qscalibrate.cpp)
# Built by default whenever the tests are: tool_qscalibrate.py skips itself when
# the binary is absent, and a permanently-skipped test reads exactly like a
# passing one in a gate summary. Kept out of a tests-off build, where nothing
# looks for it.
if(NOT BUILD_TESTS)
  set_target_properties(qscalibrate PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()
# quicksilver_cuckatoo supplies blake2b (vendor/blake2b-ref.c), which the vendored
# setheader() in every generated translation unit calls.
target_link_libraries(qscalibrate PRIVATE
  quicksilver_cuckatoo_bench
  quicksilver_cuckatoo
)
