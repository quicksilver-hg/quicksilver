# Mining-only GPU Cuckatoo solver build (Quicksilver #5c bridge).
#
# Separate from the node's CMake/consensus build: nvcc compiles the wrapper
# against the TRACKED vendored CUDA solver in ../vendor. CUDA never enters the
# node build. Built per graph size; GPU_ARCH targets the local card.
#
#   make            # qsgpusolve for EDGEBITS=28, sm_61 (GTX 1050 Ti)
#   make GPU_ARCH=sm_52        # GTX 950
#
# The produced binary honours the node parser contract (see qsgpusolve.cu).
# A single E28 binary serves both block PoW and per-tx PoW.

NVCC      ?= nvcc -std=c++14
GPU_ARCH  ?= sm_61
EDGEBITS  ?= 28
VENDOR    := ../vendor
BLAKE_SRC := $(VENDOR)/blake2b-ref.c
SRC_ROOT  := ../../..

COMMON_DEPS := $(VENDOR)/lean.cu $(VENDOR)/siphash.cuh \
        $(VENDOR)/cuckatoo.h $(VENDOR)/graph.hpp $(BLAKE_SRC) \
        device_health.cuh Makefile
DEPS := qsgpusolve.cu $(COMMON_DEPS)

qsgpusolve: $(DEPS)
	$(NVCC) -I$(SRC_ROOT) -o $@ -DEDGEBITS=$(EDGEBITS) -arch $(GPU_ARCH) qsgpusolve.cu $(BLAKE_SRC)

# Calibration-only twin of qsgpusolve: times every graph instead of stopping at
# the first cycle. Stage 1 of the tx-cost flag day (M2/M3). Not a mining tool and
# not on any validation path; see qsgpucalibrate.cu for why qscalibrate cannot
# stand in for it.
qsgpucalibrate: qsgpucalibrate.cu $(COMMON_DEPS)
	$(NVCC) -I$(SRC_ROOT) -o $@ -DEDGEBITS=$(EDGEBITS) -arch $(GPU_ARCH) qsgpucalibrate.cu $(BLAKE_SRC)

.PHONY: clean
clean:
	rm -f qsgpusolve qsgpucalibrate
