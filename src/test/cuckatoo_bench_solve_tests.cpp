// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <crypto/cuckatoo/bench/registry.h>
#include <crypto/cuckatoo/cuckatoo.h>

#include <boost/test/unit_test.hpp>

#include <cstring>
#include <vector>

BOOST_AUTO_TEST_SUITE(cuckatoo_bench_solve_tests)

namespace {
//! Place `nonce` in the trailing 4 little-endian bytes, exactly as solve_one does.
std::vector<unsigned char> KeyedPrepow(const std::vector<unsigned char>& prepow,
                                       uint32_t nonce)
{
    std::vector<unsigned char> buf = prepow;
    const size_t len = buf.size();
    buf[len - 4] = (unsigned char)(nonce & 0xff);
    buf[len - 3] = (unsigned char)((nonce >> 8) & 0xff);
    buf[len - 2] = (unsigned char)((nonce >> 16) & 0xff);
    buf[len - 1] = (unsigned char)((nonce >> 24) & 0xff);
    return buf;
}

//! Search up to `max_graphs` graphs at `edgebits`, returning the winning nonce.
//! Reports -1 if none held a 42-cycle.
int64_t FindOneCycle(const cuckatoo::bench::SolverVTable& vt, unsigned threads,
                     const std::vector<unsigned char>& prepow, uint32_t max_graphs,
                     cuckatoo::Cycle& out)
{
    void* ctx = vt.create(threads);
    if (ctx == nullptr) return -1;
    int64_t winning = -1;
    for (uint32_t nonce = 0; nonce < max_graphs && winning < 0; ++nonce) {
        if (vt.solve_one(ctx, prepow.data(), prepow.size(), nonce, out)) {
            winning = nonce;
        }
    }
    vt.destroy(ctx);
    return winning;
}

// F-254: trimming must cover every edge-bitmap word even when nthreads does
// not divide NEDGES/64. The observable contract is thread-count independence:
// solve_one at 1 thread (always covers the whole bitmap) and at 12 threads
// (8 words / 512 edges of tail were skipped before the fix at E19) must agree
// on whether a cycle was found and, if so, on the cycle; any reported cycle
// must verify. A check that only runs when 12 threads return a cycle is
// vacuous after the fix, because the pinned tail-cycle graph then reports none.
void RequireSolveAgreesAt1And12(const cuckatoo::bench::SolverVTable& vt,
                                const std::vector<unsigned char>& prepow,
                                uint32_t nonce)
{
    cuckatoo::Cycle c1{}, c12{};
    void* ctx1 = vt.create(1);
    BOOST_REQUIRE(ctx1 != nullptr);
    const bool found1 = vt.solve_one(ctx1, prepow.data(), prepow.size(), nonce, c1);
    vt.destroy(ctx1);

    void* ctx12 = vt.create(12);
    BOOST_REQUIRE(ctx12 != nullptr);
    const bool found12 = vt.solve_one(ctx12, prepow.data(), prepow.size(), nonce, c12);
    vt.destroy(ctx12);

    BOOST_CHECK_EQUAL(found1, found12);
    if (!found1 || !found12) return;
    BOOST_CHECK(c1 == c12);
    const auto keyed = KeyedPrepow(prepow, nonce);
    BOOST_CHECK_MESSAGE(vt.verify_one(c1, keyed.data(), keyed.size()),
                        "nondivisor thread count returned a cycle the verifier rejects");
}
} // namespace

// Every size the calibration sweep needs must be built and registered, plus E19,
// which exists only to tie the harness to the consensus verifier.
BOOST_AUTO_TEST_CASE(all_swept_sizes_are_registered)
{
    BOOST_CHECK_MESSAGE(cuckatoo::bench::Lookup(19) != nullptr,
                        "no bench solver registered for edgebits 19 "
                        "(needed for the consensus-verifier tie)");
    for (uint8_t bits = 22; bits <= 29; ++bits) {
        BOOST_CHECK_MESSAGE(cuckatoo::bench::Lookup(bits) != nullptr,
                            "no bench solver registered for edgebits " << int(bits));
    }
}

// THE gate on trusting any measurement from this harness. At E19 both the bench
// solver and the CONSENSUS verifier exist, so a cycle can be checked against the
// code the network actually runs. If this fails, every timing the harness
// produces is worthless, because the thing being timed is not Cuckatoo.
BOOST_AUTO_TEST_CASE(found_cycle_verifies_against_consensus_at_e19)
{
    const auto* vt = cuckatoo::bench::Lookup(19);
    BOOST_REQUIRE(vt != nullptr);

    std::vector<unsigned char> prepow(80, 0);
    std::memcpy(prepow.data(), "quicksilver-calibration-vector", 30);

    cuckatoo::Cycle cyc{};
    // 1/42 per graph; 800 graphs makes a miss vanishingly unlikely at E19,
    // where a graph costs well under a millisecond.
    const int64_t winning = FindOneCycle(*vt, 2, prepow, 800, cyc);
    BOOST_REQUIRE_MESSAGE(winning >= 0, "no 42-cycle in 800 graphs at E19");

    const auto buf = KeyedPrepow(prepow, (uint32_t)winning);
    const cuckatoo::Keys keys =
        cuckatoo::CuckatooSetHeader(buf.data(), (uint32_t)buf.size());

    BOOST_CHECK_MESSAGE(cuckatoo::CuckatooVerify(cyc, keys, 19),
                        "bench solver produced a cycle the CONSENSUS verifier "
                        "rejects at E19 — no timing from this harness is usable");
}

// Pinned E19 graph recovered from the original probe: 38-byte pre-pow
// 85 1b b7 + c5 * 35, nonce 9. Before the fix, 12 threads return a cycle
// whose last edges are 524201 and 524209 (both >= tail_start 523776) and
// 1 thread does not, so the agreement check fails on ce04fc1e. After the
// fix both thread counts report no cycle.
BOOST_AUTO_TEST_CASE(nondivisor_thread_count_agrees_on_pinned_tail_graph)
{
    const auto* vt = cuckatoo::bench::Lookup(19);
    BOOST_REQUIRE(vt != nullptr);

    std::vector<unsigned char> prepow(38, 0xc5);
    prepow[0] = 0x85;
    prepow[1] = 0x1b;
    prepow[2] = 0xb7;
    RequireSolveAgreesAt1And12(*vt, prepow, 9);
}

// Independent graph that still holds a real 42-cycle after a complete trim,
// so the verify branch actually executes. Header is the calibration vector
// used by qscalibrate; nonce 19211. Before the fix, 12 threads also emit
// three extra tail cycles (last edges 524252..524254) that solve_one does
// not return (it copies sols[0] only); after the fix both thread counts
// return the same verifying cycle.
BOOST_AUTO_TEST_CASE(nondivisor_thread_count_agrees_on_calibration_vector)
{
    const auto* vt = cuckatoo::bench::Lookup(19);
    BOOST_REQUIRE(vt != nullptr);

    std::vector<unsigned char> prepow(80, 0);
    std::memcpy(prepow.data(), "quicksilver-calibration-vector", 30);
    RequireSolveAgreesAt1And12(*vt, prepow, 19211);
}

// The bench verifier must agree with the consensus verifier where both exist,
// which is what licenses using it alone at 22-28.
BOOST_AUTO_TEST_CASE(bench_verifier_agrees_with_consensus_at_e19)
{
    const auto* vt = cuckatoo::bench::Lookup(19);
    BOOST_REQUIRE(vt != nullptr);

    std::vector<unsigned char> prepow(80, 0);
    std::memcpy(prepow.data(), "quicksilver-calibration-vector", 30);

    cuckatoo::Cycle cyc{};
    const int64_t winning = FindOneCycle(*vt, 2, prepow, 800, cyc);
    BOOST_REQUIRE(winning >= 0);

    const auto buf = KeyedPrepow(prepow, (uint32_t)winning);
    const cuckatoo::Keys keys =
        cuckatoo::CuckatooSetHeader(buf.data(), (uint32_t)buf.size());

    BOOST_CHECK_EQUAL(vt->verify_one(cyc, buf.data(), buf.size()),
                      cuckatoo::CuckatooVerify(cyc, keys, 19));

    // And it must reject a corrupted cycle, or agreement above is vacuous.
    cuckatoo::Cycle tampered = cyc;
    tampered[0] ^= 1;
    BOOST_CHECK(!vt->verify_one(tampered, buf.data(), buf.size()));
}

// A cycle reported at a swept size must verify at that same size. The consensus
// verifier has no E22 instantiation, so this uses the vendored verifier compiled
// at E22 — licensed by the E19 agreement test above.
BOOST_AUTO_TEST_CASE(found_cycle_verifies_at_e22)
{
    const auto* vt = cuckatoo::bench::Lookup(22);
    BOOST_REQUIRE(vt != nullptr);

    std::vector<unsigned char> prepow(80, 0);
    std::memcpy(prepow.data(), "quicksilver-calibration-vector", 30);

    cuckatoo::Cycle cyc{};
    // 1/42 per graph: 400 graphs makes a miss vanishingly unlikely at E22,
    // where a graph costs milliseconds.
    const int64_t winning = FindOneCycle(*vt, 2, prepow, 400, cyc);
    BOOST_REQUIRE_MESSAGE(winning >= 0, "no 42-cycle in 400 graphs at E22");

    const auto buf = KeyedPrepow(prepow, (uint32_t)winning);
    BOOST_CHECK(vt->verify_one(cyc, buf.data(), buf.size()));

    // The consensus verifier does not cover E22 — assert that, so a later change
    // that quietly widens its dispatch set is caught here rather than silently
    // turning this bench-only path into a consensus path.
    const cuckatoo::Keys keys =
        cuckatoo::CuckatooSetHeader(buf.data(), (uint32_t)buf.size());
    BOOST_CHECK(!cuckatoo::CuckatooVerify(cyc, keys, 22));
}

// solve_one searches exactly one graph. Same nonce, same answer, every time.
BOOST_AUTO_TEST_CASE(solve_one_is_deterministic_at_e22)
{
    const auto* vt = cuckatoo::bench::Lookup(22);
    BOOST_REQUIRE(vt != nullptr);

    std::vector<unsigned char> prepow(80, 7);
    void* ctx = vt->create(1);
    BOOST_REQUIRE(ctx != nullptr);

    cuckatoo::Cycle a{}, b{};
    const bool first = vt->solve_one(ctx, prepow.data(), prepow.size(), 11, a);
    const bool second = vt->solve_one(ctx, prepow.data(), prepow.size(), 11, b);
    vt->destroy(ctx);

    BOOST_CHECK_EQUAL(first, second);
    if (first) BOOST_CHECK(a == b);
}

BOOST_AUTO_TEST_SUITE_END()
