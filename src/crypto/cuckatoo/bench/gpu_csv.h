// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Calibration-only: re-check the cycles reported by the GPU harness
// (gpu/qsgpucalibrate.cu) using the CPU solver family. NEVER linked into node
// binaries.
//
// The GPU solver already verifies internally before recording a solution
// (vendor/lean.cu), but that is the same code family that produced it. A graph
// size newly built for this sweep is exactly where a solver defect would hide,
// and a timing sweep over a broken solver looks perfectly healthy. This provides
// the independent check: the graph is re-derived from the row's own nonce and
// the cycle is re-verified on the CPU.
#ifndef QUICKSILVER_CRYPTO_CUCKATOO_BENCH_GPU_CSV_H
#define QUICKSILVER_CRYPTO_CUCKATOO_BENCH_GPU_CSV_H

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace cuckatoo {
namespace bench {

//! Outcome of checking one GPU calibration CSV.
struct GpuCsvVerdict {
    size_t rows{0};               //!< data rows read
    size_t found{0};              //!< rows claiming a cycle
    size_t verified{0};           //!< claimed cycles that re-verified on CPU
    size_t consensus_checked{0};  //!< subset also checked by CuckatooVerify (E19/E28)
    std::vector<uint32_t> failed_nonces;  //!< claimed cycles that did NOT verify
    //! Non-empty if the file could not be interpreted at all. Distinct from a
    //! verification failure: a malformed file has been checked for nothing, and
    //! must not read downstream as "no failures".
    std::string error;
};

//! Check every cycle in a qsgpucalibrate CSV against `prepow` (the pre-image the
//! run was launched with; the nonce is taken from each row).
GpuCsvVerdict VerifyGpuCsv(std::istream& in, const std::vector<unsigned char>& prepow);

} // namespace bench
} // namespace cuckatoo

#endif // QUICKSILVER_CRYPTO_CUCKATOO_BENCH_GPU_CSV_H
