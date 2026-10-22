// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/mining_service.h>

#include <addresstype.h>
#include <chain.h>
#include <common/args.h>
#include <consensus/params.h>
#include <crypto/cuckatoo/cuckatoo.h>
#include <crypto/cuckatoo/gpu_solver.h>
#include <crypto/cuckatoo/multi_device.h>
#include <interfaces/mining.h>
#include <interfaces/types.h>
#include <key_io.h>
#include <logging.h>
#include <node/block_solve.h>
#include <node/miner.h>
#include <node/types.h>
#include <pow.h>
#include <primitives/block.h>
#include <uint256.h>
#include <util/check.h>
#include <util/signalinterrupt.h>
#include <util/strencodings.h>
#include <util/thread.h>
#include <util/time.h>
#include <util/translation.h>
#include <validation.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>

namespace node {

void AttemptRateWindow::Record(SteadySeconds now)
{
    const int64_t second{now.time_since_epoch().count()};
    LOCK(m_mutex);
    if (!m_last) {
        m_buckets.fill(0);
        m_first = second;
    } else if (second > *m_last) {
        // Zero the seconds skipped since the last attempt, so a bucket cannot be
        // read as current when it holds a count from a whole window ago. Never
        // more than the window's worth of them, however long the gap was.
        for (int64_t t = std::max(*m_last + 1, second - WINDOW_SECONDS + 1); t <= second; ++t) {
            m_buckets[Index(t)] = 0;
        }
    } else if (second <= *m_last - WINDOW_SECONDS) {
        return;  // older than anything the ring can still represent
    }
    m_last = m_last ? std::max(second, *m_last) : second;
    m_buckets[Index(second)] += 1;
}

std::optional<double> AttemptRateWindow::Rate(SteadySeconds now) const
{
    LOCK(m_mutex);
    if (!m_last) return std::nullopt;  // nothing observed yet: warming up, not zero
    // A caller reading a moment behind the recording thread must not produce a
    // negative span; the newest attempt is the earliest "now" that makes sense.
    const int64_t second{std::max<int64_t>(now.time_since_epoch().count(), *m_last)};
    const int64_t lo{std::max(*m_first, second - WINDOW_SECONDS + 1)};
    if (*m_last < lo) {
        // Nothing inside the window. One attempt is known to have happened at
        // m_last and none since, so the rate is at most 1/(elapsed since it).
        return 1.0 / double(second - *m_last + 1);
    }
    uint64_t attempts{0};
    for (int64_t t = lo; t <= *m_last; ++t) attempts += m_buckets[Index(t)];
    return double(attempts) / double(second - lo + 1);
}

void AttemptRateWindow::Reset()
{
    LOCK(m_mutex);
    m_buckets.fill(0);
    m_first.reset();
    m_last.reset();
}

bool CpuBlockFallbackEnabled(uint8_t edgebits)
{
    return CpuBlockMiningAllowed(edgebits, gArgs.GetBoolArg("-allowcpumining", false));
}

bool BlockSolvingPossible(uint8_t edgebits)
{
    return cuckatoo::GpuHelperConfigured(edgebits) || CpuBlockFallbackEnabled(edgebits);
}

MiningService::MiningService(ChainstateManager& chainman, interfaces::Mining& mining)
    : m_chainman(chainman), m_mining(mining) {}

MiningService::~MiningService() { Stop(); }

bool MiningService::Start(const CScript& payout_script, const std::string& payout_address)
{
    LOCK(m_thread_mutex);
    if (m_active.load()) return false;  // already active
    // Interrupt() may have asked the previous worker to exit without joining.
    // Join it before spawning a replacement: assigning to a joinable std::thread
    // calls std::terminate.
    if (m_thread.joinable()) m_thread.join();
    m_interrupt.store(false);
    // Initialize the session snapshot before the worker is observable, so a
    // startmining RPC that reads GetStatus() right after Start() sees the payout
    // address and start time (not an empty pre-thread status).
    {
        LOCK(m_stats_mutex);
        m_status = MiningStatus{};
        m_status.active = true;
        m_status.address = payout_address;
        m_status.payout_script = HexStr(payout_script);
        m_status.start_time = GetTime();
        // Publish the permit before the thread is observable. Run() reads it
        // again for the solve itself; both reads are live args, so a change
        // made after the previous arming is already in this snapshot.
        m_status.block_solving_possible = BlockSolvingPossible(m_chainman.GetConsensus().nEdgeBits);
        m_attempts.store(0, std::memory_order_relaxed);
        m_rate.Reset();
    }
    // m_active must be true before the worker can observe it: Run() exits as
    // soon as it sees false. Set it before constructing the thread, and clear
    // it if construction throws so a failed start cannot leave the service
    // stuck "active" with no worker.
    m_active.store(true);
    try {
        // Spawn through util::TraceThread, as every other long-lived thread in the
        // tree does. Run() calls createNewBlock(), getBlock(), getTip() and
        // SolveBlockPoW(), any of which can throw; an exception escaping a bare
        // thread entry point calls std::terminate() with no diagnostic at all --
        // on Windows a __fastfail exiting 0xC0000409, indistinguishable from a /GS
        // stack-cookie failure. TraceThread logs the exception before rethrowing,
        // and renames the thread, which is why Run() no longer does so itself.
        m_thread = std::thread(&util::TraceThread, "miner",
                               [this, payout_script] { Run(payout_script); });
    } catch (...) {
        m_active.store(false);
        m_interrupt.store(true);
        LOCK(m_stats_mutex);
        m_status = MiningStatus{};
        throw;
    }
    return true;
}

void MiningService::Interrupt()
{
    LOCK(m_thread_mutex);
    m_active.store(false);
    m_interrupt.store(true);
}

void MiningService::Stop()
{
    LOCK(m_thread_mutex);
    m_active.store(false);
    m_interrupt.store(true);
    if (m_thread.joinable()) m_thread.join();
}

MiningStatus MiningService::GetStatus() const
{
    MiningStatus s;
    {
        LOCK(m_stats_mutex);
        s = m_status;
    }
    s.active = m_active.load();  // atomic is authoritative for liveness
    s.attempts = m_attempts.load(std::memory_order_relaxed);
    return s;
}

std::optional<double> MiningService::RecentAttemptRate() const
{
    return m_rate.Rate(Now<SteadySeconds>());
}

namespace {
//! "First Card (pci:0000:04:00.0): <SolverFault sentence>", the card by name when discovery lists it.
std::string CardFaultText(const cuckatoo::GpuDiscovery& discovery, const std::string& id, cuckatoo::GpuSolveStatus status)
{
    const auto card = std::find_if(discovery.listing.devices.begin(), discovery.listing.devices.end(),
                                   [&id](const auto& device) { return device.id == id; });
    const std::string name = card == discovery.listing.devices.end() ? id : card->name;
    return name + " (" + id + "): " + cuckatoo::SolverFault(status, false).value_or("");
}
} // namespace

void MiningService::RecordSolverOutcome(cuckatoo::GpuSolveStatus status, bool cpu_fallback,
                                        const std::vector<cuckatoo::GpuDeviceFault>& gpu_faults)
{
    auto fault = cuckatoo::SolverFault(status, cpu_fallback);
    if (!gpu_faults.empty()) {
        const auto& first = gpu_faults.front();
        fault = CardFaultText(cuckatoo::DiscoverGpus(m_chainman.GetConsensus().nEdgeBits), first.device, first.status);
    }
    LOCK(m_stats_mutex);
    m_status.solver_ok = !fault.has_value();
    m_status.last_solver_error = fault.value_or("");
    m_status.solver_fault_from_card = !gpu_faults.empty();
    m_status.solver_missing = cuckatoo::SolverMissing(status, cpu_fallback);
}

void MiningService::Run(CScript payout_script)
{
    // Session snapshot (address/start_time/counters) is initialized in Start()
    // before this thread becomes observable; see the race note there.

    const Consensus::Params& cparams = m_chainman.GetConsensus();
    // Once per arming, from the live args. Not cached at process start: the
    // desktop writes -allowcpumining into rw settings without restarting, and
    // the next Start() runs this again.
    const bool cpu_fallback = CpuBlockFallbackEnabled(cparams.nEdgeBits);
    const bool gpu_configured = cuckatoo::GpuHelperConfigured(cparams.nEdgeBits);
    if (gpu_configured) LogInfo(HgLog::FORGE, "%s\n", cuckatoo::DescribeDiscovery(cuckatoo::DiscoverGpus(cparams.nEdgeBits)));
    {
        LOCK(m_stats_mutex);
        m_status.block_solving_possible = gpu_configured || cpu_fallback;
    }
    if (cparams.nEdgeBits != 19 && !gpu_configured) {
        if (cpu_fallback) {
            LogInfo(HgLog::FORGE, "up solver=cpu edgebits=%d warn=cpu-mining-far-slower-than-gpu\n", cparams.nEdgeBits);
        } else {
            LogInfo(HgLog::FORGE, "halted reason=no-solver edgebits=%d fix=install-quicksilver-gpu-or--cuckatoodevice-or--allowcpumining\n", cparams.nEdgeBits);
        }
    }

    while (m_active.load() && !m_interrupt.load() && !m_chainman.m_interrupt) {
        if (node::MiningShouldWaitForSync(m_chainman)) {
            if (auto tip = m_mining.getTip()) {
                m_mining.waitTipChanged(tip->hash, std::chrono::seconds(1));
            } else {
                UninterruptibleSleep(std::chrono::milliseconds(500));
            }
            continue;
        }

        // Take a counter on both sides of assembly. If the pool changes while
        // CreateNewBlock is selecting transactions, conservatively rebuild: the
        // completed template may have observed either side of that update.
        const unsigned int pool_updates_before = m_mining.getTransactionsUpdated();
        std::unique_ptr<interfaces::BlockTemplate> tmpl =
            m_mining.createNewBlock(node::BlockCreateOptions{.coinbase_output_script = payout_script});
        if (!tmpl) {
            UninterruptibleSleep(std::chrono::milliseconds(500));
            continue;
        }

        const unsigned int template_pool_updates = m_mining.getTransactionsUpdated();
        if (template_pool_updates != pool_updates_before) continue;

        CBlock block = tmpl->getBlock();
        const auto tip_before = m_mining.getTip();
        const uint256 parent = tip_before ? tip_before->hash : uint256{};
        // A later counter change means the template no longer represents the
        // pool and is cheap to supersede: Cuckatoo nonce attempts do not
        // accumulate useful state across a header.

        // Publish what is actually being ground, before the first sweep. vtx[0]
        // is the coinbase, which is not a transfer anybody is waiting on.
        {
            LOCK(m_stats_mutex);
            m_status.template_height = tip_before ? tip_before->height + 1 : 0;
            m_status.template_transactions = static_cast<int64_t>(block.vtx.size()) - 1;
        }

        while (m_active.load() && !m_interrupt.load() && !m_chainman.m_interrupt) {
            {
                LOCK(::cs_main);
                // F-479: this window's header time is now, not when the template was built.
                if (const CBlockIndex* pindexPrev = m_chainman.m_blockman.LookupBlockIndex(parent)) {
                    UpdateTime(&block, cparams, pindexPrev);
                }
            }

            uint64_t budget = 4096;
            cuckatoo::GpuSolveStatus solver_status{cuckatoo::GpuSolveStatus::kNoCycle};
            std::vector<cuckatoo::GpuDeviceFault> gpu_faults;
            bool template_changed{false};
            const bool solved = node::SolveBlockPoW(
                m_chainman, block, budget, cpu_fallback,
                /*abandon=*/[this, &parent, template_pool_updates, &template_changed] {
                    if (m_interrupt.load() || !m_active.load()) {
                        template_changed = true;
                        return true;
                    }
                    const auto t = m_mining.getTip();
                    template_changed = (t && t->hash != parent) ||
                        m_mining.getTransactionsUpdated() != template_pool_updates;
                    return template_changed;
                },
                /*on_attempt=*/[this] {
                    m_attempts.fetch_add(1, std::memory_order_relaxed);
                    m_rate.Record(Now<SteadySeconds>());
                },
                /*out_status=*/&solver_status, /*out_faults=*/&gpu_faults);

            // Publish the solver's health every window, not only on failure: a
            // card that comes back must clear the error it left behind.
            RecordSolverOutcome(solver_status, cpu_fallback, gpu_faults);

            if (solved) {
                auto block_ptr = std::make_shared<const CBlock>(block);
                if (m_chainman.ProcessNewBlock(block_ptr, /*force_processing=*/true,
                                               /*min_pow_checked=*/true, nullptr)) {
                    const CAmount minted = block_ptr->vtx[0]->GetValueOut();
                    LOCK(m_stats_mutex);
                    m_status.blocks_found += 1;
                    m_status.coins_minted_session += minted;
                    m_status.last_block_time = GetTime();
                } else {
                    LogInfo(HgLog::FORGE, "rejected reason=processnewblock-refused\n");
                }
                break;  // rebuild a fresh template
            }

            // Not solved this budget: abandon if a block arrived on our parent or
            // the relay pool moved. SolveBlockPoW rate-limits the predicate to one
            // atomic counter read per 250 ms and latches true within a sweep.
            const auto tip_now = m_mining.getTip();
            if (template_changed || (tip_now && tip_now->hash != parent) ||
                m_mining.getTransactionsUpdated() != template_pool_updates) break;
        }
    }

    LOCK(m_stats_mutex);
    m_status.active = false;
    // Nothing is being ground once the worker leaves, so stop describing a block.
    m_status.template_height = 0;
    m_status.template_transactions = 0;
}

interfaces::MiningStatus BuildMiningStatus(const MiningService& svc, ChainstateManager& chainman)
{
    const MiningStatus s = svc.GetStatus();
    const int64_t now = GetTime();
    const int64_t elapsed = s.start_time ? (now - s.start_time) : 0;

    interfaces::MiningStatus out;
    out.active = s.active;
    out.address = s.address;
    out.payout_script = s.payout_script;
    out.blocks_found = s.blocks_found;
    out.coins_minted_session = s.coins_minted_session;
    out.attempts_per_second = svc.RecentAttemptRate();
    const auto stats = cuckatoo::GetGpuDeviceStats();
    if (!stats.empty()) {
        const auto discovery = cuckatoo::DiscoverGpus(chainman.GetConsensus().nEdgeBits);
        double total_rate{0.0};
        bool have_rate{false};
        for (const auto& stat : stats) {
            interfaces::MiningStatus::Device device;
            device.id = stat.device;
            const auto card = std::find_if(discovery.listing.devices.begin(), discovery.listing.devices.end(),
                                           [&stat](const auto& found) { return found.id == stat.device; });
            if (card != discovery.listing.devices.end()) device.name = card->name;
            switch (stat.state) {
            case cuckatoo::GpuDeviceState::kIdle: device.state = "idle"; break;
            case cuckatoo::GpuDeviceState::kSolving: device.state = "solving"; break;
            case cuckatoo::GpuDeviceState::kFaulted: device.state = "faulted"; break;
            }
            if (stat.state == cuckatoo::GpuDeviceState::kSolving && stat.mean_seconds_per_graph > 0) {
                device.attempts_per_second = 1.0 / stat.mean_seconds_per_graph;
                total_rate += *device.attempts_per_second;
                have_rate = true;
            }
            if (stat.state == cuckatoo::GpuDeviceState::kFaulted) {
                device.last_error = cuckatoo::SolverFault(stat.last_status, false).value_or("");
            }
            out.devices.push_back(std::move(device));
        }
        if (have_rate) out.attempts_per_second = total_rate;
    }
    out.last_block_time = s.last_block_time;
    out.elapsed_seconds = elapsed;
    // Report the bridge this process can actually launch. The persisted option
    // updates immediately, but CUCKATOO_GPU_SOLVER intentionally changes only
    // at startup; reading gArgs here made this row claim "GPU bridge" while the
    // next solve attempt and its health row still correctly reported no solver.
    out.gpu_solver = cuckatoo::GpuHelperConfigured(chainman.GetConsensus().nEdgeBits);
    out.graphs_attempted = s.attempts;
    out.solver_ok = s.solver_ok;
    out.last_solver_error = s.last_solver_error;
    // F-534: a card's health is read from the stats table, which marks it faulted
    // the moment its helper fails. The window it faulted in is only recorded when
    // the whole window returns (4096 attempts, a new template or a cancel), which
    // on one card left mining can be a long time; and the next window has the card
    // solving again well before that window is recorded. Faults that aren't a
    // card's (no solver, processor refused) are still the window's.
    const auto faulted_card = std::find_if(stats.begin(), stats.end(), [](const auto& stat) {
        return stat.state == cuckatoo::GpuDeviceState::kFaulted;
    });
    if (faulted_card != stats.end()) {
        out.solver_ok = false;
        out.last_solver_error = CardFaultText(cuckatoo::DiscoverGpus(chainman.GetConsensus().nEdgeBits),
                                              faulted_card->device, faulted_card->last_status);
    } else if (s.solver_fault_from_card && !stats.empty()) {
        out.solver_ok = true;
        out.last_solver_error.clear();
    }
    out.solver_missing = s.solver_missing;
    // Idle has no arming snapshot. The struct default is true, which made an
    // unarmed node with no solver and processor mining off read as CPU. Ask
    // the live permit while nothing is armed: the desktop writes
    // -allowcpumining without a restart, and the next reading has to follow
    // it. While the worker is active, keep the snapshot Start() published.
    // That is the permit the running search is actually using.
    out.block_solving_possible = s.active
        ? s.block_solving_possible
        : BlockSolvingPossible(chainman.GetConsensus().nEdgeBits);
    out.template_height = s.template_height;
    out.template_transactions = s.template_transactions;
    {
        LOCK(cs_main);
        const CBlockIndex& tip = *CHECK_NONFATAL(chainman.ActiveChain().Tip());
        out.congestion_multiplier = double(tip.m_congestion) / double(CONGESTION_ONE);
        out.base_tx_work = BaseTxWork(&tip, chainman.GetConsensus()).getdouble();
    }
    return out;
}

util::Result<void> StartMining(MiningService& svc, const std::string& payout_address)
{
    const CTxDestination dest = DecodeDestination(payout_address);
    if (!IsValidDestination(dest)) {
        return util::Error{_("Invalid payout address")};
    }
    const bool started = svc.Start(GetScriptForDestination(dest), payout_address);
    if (!started && svc.GetStatus().address != payout_address) {
        return util::Error{_("Mining already active with a different payout address; stopmining first")};
    }
    return {};
}

util::Result<void> StartMining(MiningService& svc, const CScript& payout_script)
{
    const std::string script_hex{HexStr(payout_script)};
    const bool started = svc.Start(payout_script, /*payout_address=*/"");
    const MiningStatus status{svc.GetStatus()};
    if (!started && (!status.address.empty() || status.payout_script != script_hex)) {
        return util::Error{_("Mining already active with a different payout target; stopmining first")};
    }
    return {};
}

bool MiningShouldWaitForSync(const ChainstateManager& chainman)
{
    if (chainman.m_blockman.LoadingBlocks()) return true;
    LOCK(cs_main);
    const CBlockIndex* tip = chainman.ActiveChain().Tip();
    if (tip == nullptr) return true;  // no tip yet — nothing to build on
    const CBlockIndex* best = chainman.m_best_header;
    // A more-work header chain is known but not connected → sync first.
    return best != nullptr && best->nChainWork > tip->nChainWork;
}

} // namespace node
