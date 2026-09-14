# Cuckoo (cuckatoo) timing driver — Tier 0 PoW Harness, Task 8.
#
# Builds two native artifacts:
#   1. The solver binary  lean<EDGEBITS>x1  (tromp's lean.cpp, a single TU).
#      We delegate to the upstream Makefile target so it is built exactly as
#      tromp intends (same flags as the verified reference).
#   2. driver             — this directory's driver.cpp + blake2b-ref.c.
#      driver.cpp is the ONLY TU that #includes cuckatoo.h (ODR constraint:
#      cuckatoo.h defines verify/setheader/errstr NON-inline). It must NOT be
#      linked with lean.cpp.
#
# Usage:
#   make EDGEBITS=19          # builds solver (lean19x1) + driver
#   make EDGEBITS=29          # production-like; uses NSIPHASH=4 + AVX2 for solver
#   make clean
#
# How the driver finds the solver at runtime:
#   The driver derives the solver path from its own argv[0] directory:
#       <driver_dir>/../../third_party/cuckoo/src/cuckatoo/lean<EDGEBITS>x1
#   i.e. it expects to be run as ./drivers/cuckoo/driver from the repo root, or
#   from anywhere via an absolute/relative path that keeps that layout intact.
#   Override with the CUCKOO_SOLVER env var if the solver lives elsewhere.

EDGEBITS ?= 19

# Locate the upstream cuckatoo source tree relative to this Makefile.
THIS_DIR  := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
REPO_ROOT := $(abspath $(THIS_DIR)/../..)
CUCKOO_SRC := $(REPO_ROOT)/third_party/cuckoo/src/cuckatoo
CRYPTO_SRC := $(REPO_ROOT)/third_party/cuckoo/src/crypto
BLAKE_2B_SRC := $(CRYPTO_SRC)/blake2b-ref.c

# Solver flags differ by EDGEBITS, matching the upstream verified reference:
#   EDGEBITS=19 -> lean19x1 (NSIPHASH=1, scalar)
#   EDGEBITS=29 -> lean29x4 (NSIPHASH=4, -mavx2)  [Task 9 may use this]
ifeq ($(EDGEBITS),19)
SOLVER_NAME := lean19x1
SOLVER_TARGET := lean19x1
else ifeq ($(EDGEBITS),29)
SOLVER_NAME := lean29x4
SOLVER_TARGET := lean29x4
else
SOLVER_NAME := lean$(EDGEBITS)x1
SOLVER_TARGET := lean$(EDGEBITS)x1
endif

SOLVER_BIN := $(CUCKOO_SRC)/$(SOLVER_NAME)

# Driver compile flags. Mirror the cuckatoo build (INTERFACE.md): -O3, -pthread,
# NSIPHASH=1 (driver only calls verify, which is scalar regardless), and the same
# -I paths so cuckatoo.h resolves its crypto includes.
CXX ?= g++
CXXFLAGS ?= -O3 -std=c++11 -march=native -Wall -Wno-format \
            -Wno-deprecated-declarations -D_POSIX_C_SOURCE=200112L -pthread
DRIVER_DEFS := -DEDGEBITS=$(EDGEBITS) -DPROOFSIZE=42 -DNSIPHASH=1 \
               -DSOLVER_BIN_NAME='"$(SOLVER_NAME)"'
DRIVER_INCS := -I$(CUCKOO_SRC) -I$(CRYPTO_SRC)

.PHONY: all solver driver clean

all: solver driver

# Build the solver via the upstream Makefile (single TU, tromp's own flags).
solver:
	$(MAKE) -C $(CUCKOO_SRC) $(SOLVER_TARGET)

# Build the driver. The actual solver name ($(SOLVER_NAME), e.g. lean29x4 for
# EDGEBITS=29) is baked in via -DSOLVER_BIN_NAME so the driver auto-resolves it;
# CUCKOO_SOLVER env still overrides at runtime.
driver: driver.cpp $(BLAKE_2B_SRC)
	$(CXX) $(CXXFLAGS) $(DRIVER_DEFS) $(DRIVER_INCS) \
	    -o driver driver.cpp $(BLAKE_2B_SRC)

# Remove our driver and the upstream solver binary (so `make clean && make`
# rebuilds the solver too — avoids a stale solver surviving across rebuilds).
clean:
	rm -f driver
	$(MAKE) -C $(CUCKOO_SRC) clean 2>/dev/null || rm -f $(SOLVER_BIN)
