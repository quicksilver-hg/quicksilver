// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/mining_service.h>

#include <addresstype.h>
#include <chain.h>
#include <chainparams.h>
#include <common/args.h>
#include <consensus/amount.h>
#include <crypto/cuckatoo/gpu_solver.h>
#include <interfaces/mining.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <node/block_solve.h>
#include <node/types.h>
#include <primitives/block.h>
#include <script/script.h>
#include <test/util/logging.h>
#include <test/util/setup_common.h>
#include <test/util/txrelaypool.h>
#include <univalue.h>
#include <util/time.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using node::MiningService;
using node::MiningStatus;

namespace {
void SetSolverEnv(const std::string& value)
{
#ifdef WIN32
    _putenv_s("CUCKATOO_GPU_SOLVER", value.c_str());
#else
    setenv("CUCKATOO_GPU_SOLVER", value.c_str(), 1);
#endif
}

void UnsetSolverEnv()
{
#ifdef WIN32
    _putenv_s("CUCKATOO_GPU_SOLVER", "");
#else
    unsetenv("CUCKATOO_GPU_SOLVER");
#endif
}

//! A silent command that keeps the external-solver bridge occupied until the
//! mining service cancels it. The comment token consumes the solver arguments
//! appended by the bridge on each platform.
std::string BlockingSolverCommand()
{
#ifdef WIN32
    return "ping -n 3600 127.0.0.1 >NUL & rem";
#else
    return "sleep 3600 #";
#endif
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(mining_service_tests, TestChain100Setup)

BOOST_AUTO_TEST_CASE(mines_and_reports_session_subsidy)
{
    auto mining = interfaces::MakeMining(m_node);
    BOOST_REQUIRE(mining);

    const CScript payout = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    MiningService svc(*m_node.chainman, *mining);

    const int start_height = WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Height());

    BOOST_CHECK(svc.Start(payout, "test-payout"));
    BOOST_CHECK(!svc.Start(payout, "test-payout"));  // idempotent: already active

    // Sandbox fBlockPowNoCycle makes block PoW trivial; wait for a few blocks.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (svc.GetStatus().blocks_found < 3 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    svc.Stop();

    const MiningStatus st = svc.GetStatus();
    BOOST_CHECK(!st.active);
    BOOST_CHECK_GE(st.blocks_found, 3);
    BOOST_CHECK_EQUAL(st.address, "test-payout");

    // Derived expectation (never hardcoded): empty blocks pay subsidy only.
    const Consensus::Params& params = m_node.chainman->GetConsensus();
    CAmount expected = 0;
    for (int h = start_height + 1; h <= start_height + st.blocks_found; ++h) {
        expected += GetBlockSubsidy(h, params);
    }
    BOOST_CHECK_EQUAL(st.coins_minted_session, expected);

    // The chain actually advanced by blocks_found.
    const int end_height = WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Height());
    BOOST_CHECK_EQUAL(end_height, start_height + st.blocks_found);
}

BOOST_AUTO_TEST_CASE(inactive_status_is_empty)
{
    auto mining = interfaces::MakeMining(m_node);
    MiningService svc(*m_node.chainman, *mining);
    const MiningStatus st = svc.GetStatus();
    BOOST_CHECK(!st.active);
    BOOST_CHECK_EQUAL(st.blocks_found, 0);
    BOOST_CHECK_EQUAL(st.coins_minted_session, 0);
    BOOST_CHECK(st.address.empty());
}

BOOST_AUTO_TEST_CASE(build_mining_status_maps_fields)
{
    // Construct the service the same way the other cases in this file do; the
    // TestChain100Setup fixture does not populate m_node.mining_service.
    auto mining = interfaces::MakeMining(m_node);
    MiningService svc(*m_node.chainman, *mining);
    ChainstateManager& chainman = *m_node.chainman;

    // Idle service: inactive, zero telemetry, congestion at the hardware floor.
    interfaces::MiningStatus st = node::BuildMiningStatus(svc, chainman);
    BOOST_CHECK(!st.active);
    BOOST_CHECK_EQUAL(st.blocks_found, 0);
    BOOST_CHECK_EQUAL(st.coins_minted_session, 0);
    BOOST_CHECK_EQUAL(st.elapsed_seconds, 0);
    // Never armed, so no solver attempt has ever been observed. That is unknown,
    // not a rate of zero -- a zero would be indistinguishable from a running
    // miner whose card has died, which is the reading this field exists to give.
    BOOST_CHECK(!st.attempts_per_second.has_value());
    BOOST_CHECK_CLOSE(st.congestion_multiplier, 1.0, 0.001);
}

BOOST_AUTO_TEST_CASE(mining_status_reports_runtime_solver_after_setting_change)
{
    auto mining = interfaces::MakeMining(m_node);
    MiningService svc(*m_node.chainman, *mining);

    std::optional<common::SettingsValue> original_setting;
    gArgs.LockSettings([&](common::Settings& settings) {
        const auto it = settings.rw_settings.find("cuckatoosolver");
        if (it != settings.rw_settings.end()) original_setting = it->second;
        settings.rw_settings["cuckatoosolver"] = "/next-start/qsgpusolve";
    });
    const char* original_solver = std::getenv("CUCKATOO_GPU_SOLVER");
    const std::optional<std::string> original_env = original_solver
        ? std::optional<std::string>{original_solver}
        : std::nullopt;
    struct RestoreSolverState {
        std::optional<common::SettingsValue> setting;
        std::optional<std::string> environment;
        ~RestoreSolverState()
        {
            gArgs.LockSettings([&](common::Settings& settings) {
                if (setting) {
                    settings.rw_settings["cuckatoosolver"] = *setting;
                } else {
                    settings.rw_settings.erase("cuckatoosolver");
                }
            });
            if (environment) {
                SetSolverEnv(*environment);
            } else {
                UnsetSolverEnv();
            }
        }
    } restore{original_setting, original_env};

    // OptionsModel writes the new path immediately but marks it restart-only.
    // Until that restart, the execution bridge remains unset. The old status
    // path read gArgs here and claimed "GPU bridge", disagreeing with the next
    // solve attempt's kNoSolver health result.
    SetSolverEnv("");
    BOOST_REQUIRE(gArgs.IsArgSet("-cuckatoosolver"));
    BOOST_CHECK(!cuckatoo::GpuSolverPath().has_value());
    BOOST_CHECK(!node::BuildMiningStatus(svc, *m_node.chainman).gpu_solver);

    // The converse pins the source of truth too: status follows the bridge that
    // can run now, regardless of what is merely queued for the next startup.
    SetSolverEnv("/this-process/qsgpusolve");
    BOOST_REQUIRE(cuckatoo::GpuSolverPath().has_value());
    BOOST_CHECK(node::BuildMiningStatus(svc, *m_node.chainman).gpu_solver);
}

//! The window is fed explicit instants rather than a clock so every case below is
//! an exact expected value, not a tolerance around whatever the machine did.
static SteadySeconds At(int64_t s) { return SteadySeconds{std::chrono::seconds{s}}; }

BOOST_AUTO_TEST_CASE(attempt_rate_window_reports_unknown_before_any_attempt)
{
    node::AttemptRateWindow w;
    BOOST_CHECK(!w.Rate(At(0)).has_value());
    BOOST_CHECK(!w.Rate(At(1'000'000)).has_value());
}

BOOST_AUTO_TEST_CASE(attempt_rate_window_measures_the_trailing_window)
{
    node::AttemptRateWindow w;
    // One attempt per second for the full window: the rate is exactly 1/s, and
    // stays 1/s as the window slides, because every second inside it holds one.
    for (int64_t t = 0; t < node::AttemptRateWindow::WINDOW_SECONDS; ++t) w.Record(At(t));
    const int64_t last{node::AttemptRateWindow::WINDOW_SECONDS - 1};
    BOOST_CHECK_EQUAL(*w.Rate(At(last)), 1.0);

    // Two per second for the next full window: the old seconds fall out and the
    // rate is exactly 2/s, not the 1.5 a session average would still be reporting.
    for (int64_t t = node::AttemptRateWindow::WINDOW_SECONDS;
         t < 2 * node::AttemptRateWindow::WINDOW_SECONDS; ++t) {
        w.Record(At(t));
        w.Record(At(t));
    }
    BOOST_CHECK_EQUAL(*w.Rate(At(2 * node::AttemptRateWindow::WINDOW_SECONDS - 1)), 2.0);
}

BOOST_AUTO_TEST_CASE(attempt_rate_window_does_not_ramp_from_zero)
{
    // The defect being fixed: dividing by seconds-since-arming made a healthy
    // miner read far below its true rate while the arming and solver-spawn dead
    // time dominated. Here the first attempt lands at t=10 after a 10 s spawn,
    // then one every second; the reported rate must be the miner's, not the
    // session's, from the very first reading.
    node::AttemptRateWindow w;
    for (int64_t t = 10; t <= 14; ++t) w.Record(At(t));
    // Five attempts spanning seconds 10..14 inclusive: five seconds, 1.0/s. The
    // session average this replaces would have read 5/15 = 0.333 at the same
    // instant, and would still be climbing towards 1.0 minutes later.
    BOOST_CHECK_EQUAL(*w.Rate(At(14)), 1.0);
}

BOOST_AUTO_TEST_CASE(attempt_rate_window_decays_when_the_solver_goes_quiet)
{
    node::AttemptRateWindow w;
    for (int64_t t = 0; t < 60; ++t) w.Record(At(t));
    BOOST_CHECK_EQUAL(*w.Rate(At(59)), 1.0);

    // Half the window has no attempts: 60 of them now span 120 seconds.
    BOOST_CHECK_EQUAL(*w.Rate(At(119)), 0.5);

    // Every attempt has fallen out of the window. Not zero: one attempt is known
    // to have happened at t=59 and none since, so the rate is at most
    // 1/(elapsed since it) -- a bound that keeps a genuinely slow solver
    // distinguishable from a stopped one, and trends to zero either way.
    BOOST_CHECK_EQUAL(*w.Rate(At(219)), 1.0 / 161.0);
    BOOST_CHECK_EQUAL(*w.Rate(At(3659)), 1.0 / 3601.0);
    BOOST_CHECK(*w.Rate(At(3659)) > 0.0);
}

BOOST_AUTO_TEST_CASE(attempt_rate_window_survives_a_gap_longer_than_the_ring)
{
    // A solver that stops for longer than the whole window and restarts must not
    // have its old counts read as current: the buckets it lands in are the same
    // ones it used before.
    node::AttemptRateWindow w;
    for (int64_t t = 0; t < node::AttemptRateWindow::WINDOW_SECONDS; ++t) w.Record(At(t));
    const int64_t restart{10 * node::AttemptRateWindow::WINDOW_SECONDS};
    w.Record(At(restart));
    // Exactly one attempt in the trailing window. Counting the stale buckets the
    // restart landed on top of would give 121/120 instead.
    BOOST_CHECK_EQUAL(*w.Rate(At(restart)),
                      1.0 / double(node::AttemptRateWindow::WINDOW_SECONDS));
}

BOOST_AUTO_TEST_CASE(attempt_rate_window_reset_forgets_the_previous_session)
{
    node::AttemptRateWindow w;
    for (int64_t t = 0; t < 10; ++t) w.Record(At(t));
    BOOST_CHECK(w.Rate(At(9)).has_value());
    w.Reset();
    BOOST_CHECK(!w.Rate(At(9)).has_value());
    BOOST_CHECK(!w.Rate(At(1000)).has_value());
}

BOOST_AUTO_TEST_CASE(start_mining_helper_rejects_bad_address)
{
    auto mining = interfaces::MakeMining(m_node);
    MiningService svc(*m_node.chainman, *mining);
    auto res = node::StartMining(svc, "not-a-valid-address");
    BOOST_CHECK(!res);   // util::Result in error state
    BOOST_CHECK(!svc.IsActive());
}

BOOST_AUTO_TEST_CASE(start_mining_helper_starts_on_valid_address)
{
    auto mining = interfaces::MakeMining(m_node);
    MiningService svc(*m_node.chainman, *mining);
    const std::string addr = EncodeDestination(PKHash(coinbaseKey.GetPubKey()));
    auto res = node::StartMining(svc, addr);
    BOOST_CHECK(res);   // success
    BOOST_CHECK(svc.IsActive());
    svc.Stop();
}

BOOST_AUTO_TEST_CASE(wait_for_sync_predicate)
{
    ChainstateManager& chainman = *m_node.chainman;

    // At the frontier (best header == active tip): do NOT wait — this is the
    // genesis-bootstrap case (best == tip == genesis) generalized.
    BOOST_CHECK(!node::MiningShouldWaitForSync(chainman));

    // Knowing of a more-work header chain we have not connected: WAIT.
    CBlockIndex fake_best;
    CBlockIndex* saved_best;
    {
        LOCK(cs_main);
        saved_best = chainman.m_best_header;
        fake_best.nChainWork = chainman.ActiveChain().Tip()->nChainWork + 1;
        chainman.m_best_header = &fake_best;
    }
    BOOST_CHECK(node::MiningShouldWaitForSync(chainman));
    {
        LOCK(cs_main);
        chainman.m_best_header = saved_best;  // restore before fixture teardown
    }
}

BOOST_AUTO_TEST_CASE(mining_status_reports_base_tx_work_at_the_floor)
{
    // BaseTxWork = (mean block work over the MA window) / K, times the
    // congestion multiplier, floored at 1 cycle. On a fresh chain the mean
    // block work is far below K=106, so the quotient truncates to zero and the
    // floor is what survives. The harness checks the measured grind against
    // this number, so it must be exposed exactly, not approximated.
    auto mining = interfaces::MakeMining(m_node);
    BOOST_REQUIRE(mining);
    MiningService svc(*m_node.chainman, *mining);

    const interfaces::MiningStatus st =
        node::BuildMiningStatus(svc, *m_node.chainman);
    // TestChain100Setup's tip sits at the sandbox floor, so mean block work is
    // 2 cycles; 2/106 truncates to 0 and the floor is what survives. This holds
    // whatever the congestion multiplier is, since it scales a zero base.
    // The published number is the CHAIN-dependent scalar. After the Stage 2 flag day
    // there is no single "required work" at a tip -- a 400-byte payment and a 100 kB
    // bloat transaction owe different amounts -- so publishing one under that name
    // would be publishing a fiction, and it is the one number every sender reads.
    BOOST_CHECK_EQUAL(st.base_tx_work, 1.0);
}

BOOST_AUTO_TEST_CASE(solve_returns_false_when_abandoned)
{
    // A template supersedes the moment a block lands on its parent. The abandon
    // predicate is checked before any grinding, including the sandbox trivial-PoW
    // path -- otherwise a sandbox solve would succeed anyway and hide the wiring.
    auto mining = interfaces::MakeMining(m_node);
    BOOST_REQUIRE(mining);
    const CScript payout = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    auto tmpl = mining->createNewBlock(node::BlockCreateOptions{.coinbase_output_script = payout});
    BOOST_REQUIRE(tmpl);
    CBlock block = tmpl->getBlock();

    uint64_t budget = 4096;
    const bool solved = node::SolveBlockPoW(*m_node.chainman, block, budget, /*cpu_fallback=*/true,
                                            /*abandon=*/[] { return true; });
    BOOST_CHECK(!solved);
    BOOST_CHECK_EQUAL(budget, 4096u);  // returned before consuming any window
}

BOOST_AUTO_TEST_CASE(solve_proceeds_when_not_abandoned)
{
    // Guards the inverted predicate, which would silently stop all mining.
    auto mining = interfaces::MakeMining(m_node);
    BOOST_REQUIRE(mining);
    const CScript payout = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    auto tmpl = mining->createNewBlock(node::BlockCreateOptions{.coinbase_output_script = payout});
    BOOST_REQUIRE(tmpl);
    CBlock block = tmpl->getBlock();

    uint64_t budget = 4096;
    const bool solved = node::SolveBlockPoW(*m_node.chainman, block, budget, /*cpu_fallback=*/true,
                                            /*abandon=*/[] { return false; });
    BOOST_CHECK(solved);  // sandbox fBlockPowNoCycle: the trivial grind succeeds
}

BOOST_AUTO_TEST_CASE(relaypool_update_rebuilds_same_height_template)
{
    auto mining = interfaces::MakeMining(m_node);
    BOOST_REQUIRE(mining);

    const CScript payout = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    const CScript destination = GetScriptForDestination(PKHash(coinbaseKey.GetPubKey()));
    CMutableTransaction transfer = CreateValidRelayPoolTransaction(
        m_coinbase_txns[0], 0, 0, coinbaseKey, destination,
        m_coinbase_txns[0]->vout[0].nValue, /*submit=*/false);
    {
        LOCK(cs_main);
        ProveTxPowForTest(transfer, *Assert(m_node.chainman->ActiveChain().Tip()), Params().GetConsensus());
    }

    // Keep the worker on one height so the test can distinguish a relay-pool
    // rebuild from the ordinary next-block rebuild. Disable sandbox's trivial
    // block solve and hold the external solver in a cancellable command. Preserve
    // E19 so the transfer proof built above remains valid.
    Consensus::Params& params = const_cast<Consensus::Params&>(Params().GetConsensus());
    struct TestStateRestorer {
        Consensus::Params& params;
        const bool block_pow_no_cycle;
        const bool tx_pow_no_cycle;
        const std::string solver;
        ~TestStateRestorer()
        {
            params.fBlockPowNoCycle = block_pow_no_cycle;
            params.fTxPowNoCycle = tx_pow_no_cycle;
            SetSolverEnv(solver);
        }
    } restore{params, params.fBlockPowNoCycle, params.fTxPowNoCycle,
              std::getenv("CUCKATOO_GPU_SOLVER") ? std::getenv("CUCKATOO_GPU_SOLVER") : ""};
    params.fBlockPowNoCycle = false;
    params.fTxPowNoCycle = true;
    SetSolverEnv(BlockingSolverCommand());

    MiningService svc(*m_node.chainman, *mining);
    BOOST_REQUIRE(svc.Start(payout, "test-payout"));

    const auto initial_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (svc.GetStatus().template_height == 0 && std::chrono::steady_clock::now() < initial_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const MiningStatus before = svc.GetStatus();
    BOOST_REQUIRE_GT(before.template_height, 0);
    BOOST_CHECK_EQUAL(before.template_transactions, 0);
    const int chain_height = WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Height());

    TestRelayPoolEntryHelper entry;
    entry.Height(chain_height).Time(Now<NodeSeconds>()).SpendsCoinbase(true);
    AddToRelayPool(*m_node.relaypool, entry.FromTx(transfer));

    const auto refresh_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (svc.GetStatus().template_transactions != 1 && std::chrono::steady_clock::now() < refresh_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const MiningStatus after = svc.GetStatus();
    svc.Stop();

    BOOST_CHECK_EQUAL(after.template_height, before.template_height);
    BOOST_CHECK_EQUAL(after.template_transactions, 1);
    BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Height()), chain_height);
}

BOOST_AUTO_TEST_CASE(worker_thread_is_wrapped_so_an_escaping_exception_is_logged)
{
    auto mining = interfaces::MakeMining(m_node);
    BOOST_REQUIRE(mining);
    const CScript payout = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;

    // Run() executes on its own thread and calls createNewBlock(), getBlock(),
    // getTip() and SolveBlockPoW(), any of which can throw. An exception that
    // escapes a thread entry point calls std::terminate() -- on Windows that is
    // a __fastfail, which exits 0xC0000409 with no log line, no exception type
    // and no message. util::TraceThread is the project's wrapper for exactly
    // this: it logs the exception via PrintExceptionContinue before rethrowing,
    // and every other long-lived thread in the tree is spawned through it.
    //
    // Assert the wrapper is in place by the start line it emits. Reverting
    // Start() to a bare `std::thread(&MiningService::Run, ...)` removes that
    // line and fails this case. Stop() joins, so the entry point has certainly
    // run by the time it returns.
    {
        ASSERT_DEBUG_LOG("miner thread start");
        MiningService svc(*m_node.chainman, *mining);
        BOOST_REQUIRE(svc.Start(payout, "test-payout"));
        svc.Stop();
    }
}

BOOST_AUTO_TEST_CASE(concurrent_start_stop_does_not_terminate)
{
    auto mining = interfaces::MakeMining(m_node);
    BOOST_REQUIRE(mining);
    const CScript payout = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    MiningService svc(*m_node.chainman, *mining);

    // startmining and stopmining arrive on separate HTTP worker threads and
    // used to share m_thread with no mutex. Stop() sets m_active=false and
    // blocks in join(); a concurrent Start() then sees "not active" and
    // assigns to the still-joinable std::thread, which calls std::terminate.
    // The same hole: Start() used to set m_active before constructing the
    // thread, so a throwing constructor left the service permanently "active"
    // with no worker.
    //
    // This case cannot assert terminate() itself (that ends the process). It
    // asserts the lifecycle that the mutex makes possible: mixed Start/Stop
    // from several threads must complete, leave the service idle, and leave
    // it able to Start again. Reverting Start/Stop to unsynchronized
    // exchange+assign is the defect; under load that abort is how it fails.
    constexpr int kThreads = 4;
    constexpr int kIters = 25;
    std::atomic<int> starts{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kIters; ++i) {
                if (svc.Start(payout, "test-payout")) {
                    starts.fetch_add(1, std::memory_order_relaxed);
                }
                svc.Stop();
            }
        });
    }
    for (std::thread& t : threads) t.join();

    BOOST_CHECK(!svc.IsActive());
    BOOST_CHECK_GE(starts.load(), 1);
    BOOST_CHECK(svc.Start(payout, "test-payout"));
    BOOST_CHECK(svc.IsActive());
    svc.Stop();
    BOOST_CHECK(!svc.IsActive());
}

BOOST_AUTO_TEST_SUITE_END()
