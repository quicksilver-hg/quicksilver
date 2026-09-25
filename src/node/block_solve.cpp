// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/block_solve.h>

#include <arith_uint256.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <crypto/cuckatoo/cuckatoo.h>
#include <logging.h>
#include <pow.h>
#include <primitives/block.h>
#include <util/signalinterrupt.h>
#include <validation.h>

#include <algorithm>
#include <chrono>

namespace node {

// The solver entry points take a fixed-width block pre-pow but must not include
// primitives/block.h to say so. This is the fence that keeps the two definitions
// in step; if the header grows again, this is what fails first.
static_assert(cuckatoo::PREPOW_BYTES == CBlockHeader::PREPOW_SIZE,
              "cuckatoo::PREPOW_BYTES must match the CBlockHeader pre-pow width");

bool CpuBlockMiningAllowed(uint8_t edgebits, bool allow_cpu_mining)
{
    return edgebits == 19 || allow_cpu_mining;
}

bool SolveBlockPoW(ChainstateManager& chainman, CBlock& block, uint64_t& max_tries,
                   bool cpu_fallback,
                   const std::function<bool()>& abandon,
                   const std::function<void()>& on_attempt,
                   cuckatoo::GpuSolveStatus* out_status)
{
    // Every early return below must leave the caller with a defined status: the
    // abandon guard, and the sandbox fBlockPowNoCycle branch, both return before
    // any solver is reached. Neither is a fault, so kNoCycle is the right answer.
    if (out_status) *out_status = cuckatoo::GpuSolveStatus::kNoCycle;

    // Checked before the trivial-PoW branch below, so the sandbox path honours it
    // too -- and so the wiring is testable where no real sweep ever runs.
    if (abandon && abandon()) return false;

    block.hashMerkleRoot = BlockMerkleRoot(block);

    const Consensus::Params& cparams = chainman.GetConsensus();

    // Quicksilver: sandbox-only trivial block PoW (fBlockPowNoCycle). No real cycle
    // is required, so skip the expensive CuckatooSolve sweep and grind only the
    // cheap proof-hash threshold. CheckProofOfWorkImpl applies the matching skip on
    // the verify side. See consensus/params.h.
    if (cparams.fBlockPowNoCycle) {
        const auto target = DeriveTarget(block.nBits, cparams.powLimit);
        if (target) {
            for (uint32_t i = 0; i < block.nCycle.size(); ++i) block.nCycle[i] = i + 1;
            while (UintToArith256(cuckatoo::CuckatooProofHash(block.nCycle)) > *target) {
                block.nCycle[0] += block.nCycle.size();
            }
            if (CheckProofOfWork(block, cparams)) return true;
        }
        return false;
    }

    // Checking m_interrupt only between budget windows is not enough to shut down:
    // one window is up to 4096 graphs, and a single GPU attempt can block for the
    // whole no-progress watchdog. Hand the solver the same flag so it can abandon
    // the sweep from inside. Observed without this: a dead GPU held quicksilver-daemon at
    // 609% CPU for 90+ seconds after SIGTERM, and the process needed SIGKILL.
    //
    // `abandon` rides the same predicate, which is what lets a superseded template
    // die mid-cycle rather than after a full ~40-80s sweep. Poll it at most every
    // 250 ms and latch a true result: the CPU solvers call cancel once per graph,
    // an E19 graph is milliseconds, and abandon takes cs_main -- unthrottled this is
    // thousands of cs_main acquisitions per second on sandbox. The caller answers
    // "is my template stale"; how often to ask belongs here, next to the solver's
    // own polling cadence.
    constexpr auto kAbandonPollInterval = std::chrono::milliseconds(250);
    auto next_poll = std::chrono::steady_clock::now();
    bool abandoned = false;
    const auto cancel = [&]() -> bool {
        if (chainman.m_interrupt) return true;
        if (!abandon || abandoned) return abandoned;
        const auto now = std::chrono::steady_clock::now();
        if (now < next_poll) return false;
        next_poll = now + kAbandonPollInterval;
        abandoned = abandon();
        return abandoned;
    };

    cuckatoo::SolverProgressCallback progress{};
    if (on_attempt) progress = [&on_attempt](uint32_t) { on_attempt(); };

    // A dead GPU used to be indistinguishable from an unlucky nonce window: both
    // ended the attempt with no cycle and nothing said. Warn once per solve call
    // rather than per window, so the log stays readable during a long grind.
    cuckatoo::GpuSolveStatus local_status{cuckatoo::GpuSolveStatus::kNoCycle};
    cuckatoo::GpuSolveStatus& gpu_status{out_status ? *out_status : local_status};
    bool warned_no_device{false};

    // F-253: a cycle the solver produced that does not verify. The sweep resumes and the
    // block still gets a sound proof, so this is not a failure -- but it is the same
    // indistinguishable-from-an-unlucky-grind shape as the two faults above. Without it
    // the only symptom of a solver emitting false cycles is that mining got slower.
    uint32_t discarded_cycles{0};
    uint32_t warned_discarded{0};

    while (max_tries > 0 && !cancel()) {
        const uint32_t budget = static_cast<uint32_t>(std::min<uint64_t>(max_tries, 4096));
        const auto pre = block.PrePowBytes();
        cuckatoo::Cycle cyc{};
        uint32_t won = 0;
        const bool solved = cuckatoo::CuckatooSolve(pre, cparams.nEdgeBits, block.nNonce, budget,
                                                    cyc, won, cpu_fallback,
                                                    progress, cancel, &gpu_status,
                                                    &discarded_cycles);
        if (discarded_cycles > warned_discarded) {
            LogWarning(HgLog::FORGE,
                       "degraded reason=solver-cycle-failed-verify count=%u edgebits=%d fix=none-work-continues\n",
                       discarded_cycles, cparams.nEdgeBits);
            warned_discarded = discarded_cycles;
        }
        if (!warned_no_device && gpu_status == cuckatoo::GpuSolveStatus::kNoCudaDevice) {
            warned_no_device = true;
            LogWarning(HgLog::FORGE,
                       "stalled reason=no-cuda-device edgebits=%d fix=install-cuda-driver-or--allowcpumining\n",
                       cparams.nEdgeBits);
        }
        // A mid-solve fault is not a slow grind, and unlike a missing device it
        // can start hours in. Warn every time rather than once: the card may
        // recover between attempts, and a single line at the top of a long log
        // is how this stayed invisible in the first place.
        if (gpu_status == cuckatoo::GpuSolveStatus::kDeviceFault) {
            LogWarning(HgLog::FORGE,
                       "stalled reason=gpu-device-fault edgebits=%d fix=see-solver-stderr-driver-watchdog\n",
                       cparams.nEdgeBits);
        }
        if (solved) {
            block.nNonce = won;
            block.nCycle = cyc;
            if (CheckProofOfWork(block, cparams)) return true;
            block.nNonce = won + 1;  // cycle above target; keep sweeping
        } else {
            block.nNonce += budget;  // no cycle in this window
        }
        max_tries -= budget;
    }
    return false;
}

} // namespace node
