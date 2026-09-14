# Equihash timing driver — Makefile
#
# Builds driver from driver.cpp + blake/blake2b.cpp (compiled separately).
# equi.h and equi_miner.h are included only in driver.cpp (single-TU ODR rule).
# blake/blake2b.cpp does NOT include equi.h, so no ODR conflict.
#
# WN=200, WK=9 (default; n=200 k=9 Zcash-compatible parameters).
# Set at compile time via -DWN=N -DWK=K if overriding.

EQUIHASH_DIR := ../../third_party/equihash
BLAKE_DIR    := $(EQUIHASH_DIR)/blake

CXX      := g++
CXXFLAGS := -O3 -std=c++11 -march=native -pthread \
            -Wall -Wno-deprecated-declarations \
            -D_POSIX_C_SOURCE=200112L \
            -I$(EQUIHASH_DIR) \
            -I$(BLAKE_DIR)

# WN/WK defaults are in equi.h; no -DWN/-DWK needed for n=200,k=9.
# Uncomment to override:
# CXXFLAGS += -DWN=200 -DWK=9

BLAKE_SRC := $(BLAKE_DIR)/blake2b.cpp

driver: driver.cpp $(BLAKE_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $^

clean:
	rm -f driver

.PHONY: clean
