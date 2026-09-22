// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_NODE_MINING_SERVICE_H
#define QUICKSILVER_NODE_MINING_SERVICE_H

#include <consensus/amount.h>
#include <interfaces/node.h>
#include <script/script.h>
#include <sync.h>
#include <util/result.h>
#include <util/time.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>

class ChainstateManager;
namespace interfaces { class Mining; }

namespace node {

//! Trailing-window count of solver attempts, so the reported rate describes the
//! miner now rather than averaged over the whole session.
//!
//! The session average it replaces had a ramp: it divides by the seconds since
//! arming, which include the IBD wait, the first template build and the solver
//! process spawn, so a healthy miner reads far below its true rate for as long as
//! that dead time is a meaningful share of the session. It is also unable to
//! answer the question the number exists for -- a card that dies an hour in keeps
//! reporting the hour's average, and a card that comes back takes another hour to
//! say so.
//!
//! One second per bucket over WINDOW_SECONDS. Time is passed in rather than read
//! so the window is testable at exact instants; MiningService passes the steady
//! clock, which cannot step backwards under a wall-clock adjustment.
class AttemptRateWindow
{
public:
    //! Shorter than the 5-minute block interval, so a change in the miner shows up
    //! within one block; four times the solver's 30 s no-progress watchdog, so a
    //! killed solver reads as decay rather than a cliff; and long enough to hold
    //! ~75 samples at the E28 reference rate of ~1.6 s per graph.
    static constexpr int64_t WINDOW_SECONDS{120};

    //! Record one solver attempt observed at `now`.
    void Record(SteadySeconds now) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    //! Attempts per second at `now`, or nullopt when no attempt has ever been
    //! observed -- the genuinely unknown case, which callers must render as a
    //! warming-up state and never as a rate of zero.
    //!
    //! When the last attempt has fallen out of the window the result is the rate
    //! implied by that one attempt, 1/(seconds since it): the upper bound on a
    //! rate consistent with having seen nothing since. A quiet miner therefore
    //! decays towards zero instead of snapping to it, which keeps a legitimately
    //! slow solver (CPU fallback at E28 runs ~52 s per graph) distinguishable from
    //! a stopped one.
    std::optional<double> Rate(SteadySeconds now) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    //! Forget every recorded attempt; called when a mining session starts.
    void Reset() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    static size_t Index(int64_t second) { return static_cast<size_t>(((second % WINDOW_SECONDS) + WINDOW_SECONDS) % WINDOW_SECONDS); }

    mutable Mutex m_mutex;
    std::array<uint64_t, WINDOW_SECONDS> m_buckets GUARDED_BY(m_mutex){};
    //! Second of the first and most recent attempts. Unset before the first one.
    std::optional<int64_t> m_first GUARDED_BY(m_mutex);
    std::optional<int64_t> m_last GUARDED_BY(m_mutex);
};

//! Snapshot of the background mining role's state and this-session mint telemetry.
struct MiningStatus {
    bool active{false};
    std::string address;                 //!< payout address (empty if never started)
    int64_t blocks_found{0};             //!< blocks mined this session
    CAmount coins_minted_session{0};     //!< sum of coinbase value of blocks mined this session
    uint64_t attempts{0};                //!< graphs attempted this session
    int64_t start_time{0};               //!< unix seconds when the current run started (0 if never)
    int64_t last_block_time{0};          //!< unix seconds of last mined block (0 if none)
    //! Whether the last solver attempt said anything was wrong, and what. A node
    //! with a card off the PCIe bus used to advertise an active mining role and
    //! nothing else for seven hours; these two fields are what names the failure.
    bool solver_ok{true};
    std::string last_solver_error;
    //! True when the fault is specifically "no solver is configured", as opposed to a
    //! solver that ran and failed. The message alone cannot carry this: it is written
    //! for quicksilverd and names -cuckatoosolver, a flag a GUI user has no way to set.
    //! The GUI substitutes its own wording for this case and points at the control it
    //! actually has, so it needs the fault's identity, not its prose.
    bool solver_missing{false};
    //! Whether a block can be solved in this configuration: a GPU solver is
    //! configured, or CPU block mining is permitted. False is the halted state
    //! — armed, with no solver and no permitted fallback — and it is known at
    //! arming, not after the first solve attempt. Default true so an idle
    //! snapshot is not reported as halted.
    bool block_solving_possible{true};
    //! The block the worker is grinding right now, as opposed to the last block
    //! anybody assembled. BlockAssembler's currentblocktx/currentblockweight are
    //! process-wide static variables stamped by whichever caller assembled last -- an RPC
    //! getblocktemplate overwrites them without the miner's template changing at
    //! all -- so they cannot answer "is my transfer in the block being worked
    //! on?". These can: the worker publishes each template it starts grinding,
    //! including replacements triggered by relay-pool updates.
    int64_t template_height{0};          //!< height of the block being ground (0 if none)
    int64_t template_transactions{0};    //!< transactions in it, excluding the coinbase
};

//! Opt-in, off-by-default background block-mining role. Owns one worker thread that
//! builds templates, solves via the CPU/GPU dispatch seam, and submits blocks.
class MiningService
{
public:
    MiningService(ChainstateManager& chainman, interfaces::Mining& mining);
    ~MiningService();

    MiningService(const MiningService&) = delete;
    MiningService& operator=(const MiningService&) = delete;

    //! Launch the worker with the given payout target. Returns false if already active.
    bool Start(const CScript& payout_script, const std::string& payout_address)
        EXCLUSIVE_LOCKS_REQUIRED(!m_thread_mutex, !m_stats_mutex);
    //! Signal the worker to stop and join it. Safe to call when already stopped.
    void Stop() EXCLUSIVE_LOCKS_REQUIRED(!m_thread_mutex);
    //! Signal-only stop (no join); used to order shutdown before joining elsewhere.
    void Interrupt() EXCLUSIVE_LOCKS_REQUIRED(!m_thread_mutex);

    bool IsActive() const { return m_active.load(); }
    MiningStatus GetStatus() const EXCLUSIVE_LOCKS_REQUIRED(!m_stats_mutex);
    //! Solver attempts per second over the trailing window, or nullopt when no
    //! attempt has been observed this session. See AttemptRateWindow.
    std::optional<double> RecentAttemptRate() const;

private:
    void Run(CScript payout_script) EXCLUSIVE_LOCKS_REQUIRED(!m_stats_mutex);

    ChainstateManager& m_chainman;
    interfaces::Mining& m_mining;

    std::atomic<bool> m_active{false};
    std::atomic<bool> m_interrupt{false};
    //! Graphs attempted this session. Counted off the solver's per-attempt progress
    //! callback rather than the nonce budget: the budget bills 4096 for a window
    //! that finds nothing and zero for one that finds a cycle, which made a dead
    //! GPU read as 402k attempts/s. Lock-free because the CPU solver fires this
    //! once per graph.
    std::atomic<uint64_t> m_attempts{0};
    //! Same attempts, bucketed by arrival second, for the reported rate. The
    //! session counter above answers "has this solver ever produced anything";
    //! this answers "is it producing anything now", and only the second question
    //! can be asked of a number.
    AttemptRateWindow m_rate;
    //! Serializes Start/Stop/Interrupt so a concurrent startmining cannot
    //! assign m_thread while Stop is joining it (std::thread assignment to a
    //! joinable thread calls std::terminate). Taken before m_stats_mutex.
    Mutex m_thread_mutex;
    std::thread m_thread GUARDED_BY(m_thread_mutex);

    mutable Mutex m_stats_mutex;
    MiningStatus m_status GUARDED_BY(m_stats_mutex);
};

//! Compute a GUI/RPC-facing status snapshot from the live service + chain tip.
interfaces::MiningStatus BuildMiningStatus(const MiningService& svc, ChainstateManager& chainman);

//! Live read of -allowcpumining through CpuBlockMiningAllowed.
//! MiningService calls this once per arming, at the start of the mining
//! thread, not at process start. A settings change is visible on the next call.
bool CpuBlockFallbackEnabled(uint8_t edgebits);

//! True when this process can solve a block at `edgebits`: a GPU solver is
//! configured, or CpuBlockFallbackEnabled. Same live read as the fallback bit.
bool BlockSolvingPossible(uint8_t edgebits);

//! Decode an address and start the service on it. Idempotent when the service
//! is already active on the SAME address; errors when active on a different one.
util::Result<void> StartMining(MiningService& svc, const std::string& payout_address);

// Returns true if the background miner should defer instead of building on the
// current tip: the node is either still importing blocks from disk, or it knows
// of a more-work header chain it has not yet connected. Deliberately does NOT
// consult the wall-clock tip-age (max_tip_age): a correctly bootstrapping chain
// whose genesis is timestamped in the past is at the frontier and must mine.
// Takes cs_main internally; the caller must not already hold it.
bool MiningShouldWaitForSync(const ChainstateManager& chainman);

} // namespace node

#endif // QUICKSILVER_NODE_MINING_SERVICE_H
