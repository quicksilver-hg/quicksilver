// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <crypto/cuckatoo/cuckatoo.h>

#include <primitives/block.h>
#include <streams.h>

#include <array>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(cuckatoo_tests)

BOOST_AUTO_TEST_CASE(proofhash_is_deterministic_and_binds_every_edge)
{
    cuckatoo::Cycle a{};
    for (uint32_t i = 0; i < cuckatoo::PROOFSIZE; ++i) a[i] = i + 1;
    cuckatoo::Cycle b = a;
    b[41] = 99999;  // change one edge

    const uint256 ha = cuckatoo::CuckatooProofHash(a);
    const uint256 hb = cuckatoo::CuckatooProofHash(b);

    BOOST_CHECK(ha == cuckatoo::CuckatooProofHash(a));  // deterministic
    BOOST_CHECK(ha != hb);                              // every edge matters
    BOOST_CHECK(ha != uint256::ZERO);
}

BOOST_AUTO_TEST_CASE(setheader_derives_stable_keys_and_reacts_to_input)
{
    unsigned char hdr[80];
    for (int i = 0; i < 80; ++i) hdr[i] = static_cast<unsigned char>(i);
    const cuckatoo::Keys a = cuckatoo::CuckatooSetHeader(hdr, 80);
    BOOST_CHECK(cuckatoo::CuckatooSetHeader(hdr, 80).k0 == a.k0);  // deterministic
    hdr[79] ^= 0x01;
    const cuckatoo::Keys b = cuckatoo::CuckatooSetHeader(hdr, 80);
    BOOST_CHECK(a.k0 != b.k0 || a.k1 != b.k1 || a.k2 != b.k2 || a.k3 != b.k3);
}

// Golden EDGEBITS-19 solution captured from the Tromp lean19 solver (nonce 74).
// The solver prints siphash keys and the 42-edge cycle directly, so we verify
// against the keys verbatim — no header reconstruction needed.
static const cuckatoo::Keys GOLDEN19_KEYS{
    0xd23109bd4dac0bdfULL, 0x76fbe03c31ad8133ULL,
    0xd17f301a7a865e22ULL, 0xbaf57c804957a342ULL};
static const cuckatoo::Cycle GOLDEN19_CYCLE{
    0x19ce, 0xc1c9, 0xe95f, 0xfc42, 0x1034f, 0x1192c, 0x129a4, 0x12a44,
    0x12a58, 0x178c4, 0x1b393, 0x1eeb6, 0x1f047, 0x23272, 0x2449b, 0x25635,
    0x263ca, 0x2b2ce, 0x2db7e, 0x2f224, 0x3054d, 0x3468d, 0x374c4, 0x3c4e4,
    0x451e1, 0x47cbb, 0x4c7d3, 0x53b59, 0x555af, 0x560c4, 0x5a67c, 0x62c04,
    0x65f3d, 0x695e0, 0x6fa68, 0x77639, 0x7a06b, 0x7b2f2, 0x7cba1, 0x7d03e,
    0x7d5b2, 0x7f26b};

BOOST_AUTO_TEST_CASE(verify_accepts_golden_rejects_tamper_and_wrong_size)
{
    BOOST_CHECK(cuckatoo::CuckatooVerify(GOLDEN19_CYCLE, GOLDEN19_KEYS, 19));   // golden passes at 19

    cuckatoo::Cycle bad = GOLDEN19_CYCLE;
    bad[20] += 2;                                                               // perturb one edge (stays ascending)
    BOOST_CHECK(!cuckatoo::CuckatooVerify(bad, GOLDEN19_KEYS, 19));             // tamper fails

    BOOST_CHECK(!cuckatoo::CuckatooVerify(GOLDEN19_CYCLE, GOLDEN19_KEYS, 28));  // right cycle, wrong graph size (28 supported, 19-cycle invalid there)
    BOOST_CHECK(!cuckatoo::CuckatooVerify(GOLDEN19_CYCLE, GOLDEN19_KEYS, 7));   // unsupported size invalid
    BOOST_CHECK(!cuckatoo::CuckatooVerify(GOLDEN19_CYCLE, GOLDEN19_KEYS, 29));  // 29 is not a dispatched graph size
}

BOOST_AUTO_TEST_CASE(blockheader_roundtrips_cycle_and_binds_hash)
{
    CBlockHeader h;
    h.SetNull();
    h.nVersion = 1; h.nBits = 0x207fffff; h.nNonce = 7;
    for (int i = 0; i < 42; ++i) h.nCycle[i] = 1000 + i;

    DataStream ds;
    ds << h;
    // 84-byte pre-pow + 42 * 4-byte edges = 252 bytes. (Was 80/248 before nCongestion
    // entered the header in #5c-1 Phase 2 — a spec change, not a loosened bound.)
    BOOST_CHECK_EQUAL(ds.size(), 252u);

    CBlockHeader r;
    ds >> r;
    for (int i = 0; i < 42; ++i) BOOST_CHECK_EQUAL(r.nCycle[i], uint32_t(1000 + i));

    const uint256 h1 = h.GetHash();
    h.nCycle[41] = 999999;                 // a proof change must change identity
    BOOST_CHECK(h.GetHash() != h1);

    // Pre-pow excludes the cycle: 84 bytes, unaffected by nCycle.
    const auto pre = h.PrePowBytes();
    BOOST_CHECK_EQUAL(pre.size(), 84u);
}

BOOST_AUTO_TEST_CASE(solve_then_verify_roundtrips_at_19)
{
    // The solver sweeps nonces internally (writing each into the pre-pow's nNonce
    // slot, which is always the LAST four bytes) and returns the winning nonce.
    // Reconstruct the pre-pow with that nonce and confirm what we solved verifies.
    constexpr size_t kNonceOffset{cuckatoo::PREPOW_BYTES - 4};
    std::array<unsigned char, cuckatoo::PREPOW_BYTES> prepow{};
    for (size_t i = 0; i < kNonceOffset; ++i) prepow[i] = static_cast<unsigned char>(i * 7 + 1);

    cuckatoo::Cycle sol{};
    uint32_t won = 0;
    const bool found = cuckatoo::CuckatooSolve(prepow, 19, /*start_nonce=*/0, /*max_attempts=*/512, sol, won);
    BOOST_CHECK(found);
    if (found) {
        prepow[kNonceOffset + 0] = static_cast<unsigned char>(won & 0xff);
        prepow[kNonceOffset + 1] = static_cast<unsigned char>((won >> 8) & 0xff);
        prepow[kNonceOffset + 2] = static_cast<unsigned char>((won >> 16) & 0xff);
        prepow[kNonceOffset + 3] = static_cast<unsigned char>((won >> 24) & 0xff);
        const cuckatoo::Keys keys = cuckatoo::CuckatooSetHeader(prepow.data(), prepow.size());
        BOOST_CHECK(cuckatoo::CuckatooVerify(sol, keys, 19));  // what we solve, verifies
    }
}

BOOST_AUTO_TEST_CASE(solve_reports_progress_once_per_graph_attempted)
{
    // MiningService counts mining attempts off this callback, so the contract has
    // to be exact: one call per graph tried, including the winning one, none
    // missed and none doubled. The solver breaks out on success (solve_19.cpp),
    // so the count is exactly won - start_nonce + 1.
    std::array<unsigned char, cuckatoo::PREPOW_BYTES> prepow{};
    for (size_t i = 0; i < prepow.size() - 4; ++i) prepow[i] = static_cast<unsigned char>(i * 7 + 1);

    uint32_t calls = 0;
    uint32_t last_seen = 0;
    cuckatoo::Cycle sol{};
    uint32_t won = 0;
    const bool found = cuckatoo::CuckatooSolve(
        prepow, 19, /*start_nonce=*/0, /*max_attempts=*/512, sol, won, /*cpu_fallback=*/true,
        /*progress=*/[&](uint32_t nonce) { ++calls; last_seen = nonce; });

    BOOST_REQUIRE(found);
    BOOST_CHECK_EQUAL(calls, won + 1);   // start_nonce is 0
    BOOST_CHECK_EQUAL(last_seen, won);   // the last graph tried is the one that won
}

BOOST_AUTO_TEST_CASE(cuckatoo_solve_bytes_variable_length)
{
    // A tx-shaped (non-80, non-4-aligned) pre-image: 37 bytes, trailing nonce slot.
    std::vector<unsigned char> pre(37, 0xAB);
    cuckatoo::Cycle cyc{};
    uint32_t won = 0;
    const bool ok = cuckatoo::CuckatooSolveBytes(pre.data(), pre.size(), /*edgebits=*/19,
                                                 /*start=*/0, /*max=*/1u << 20, cyc, won);
    BOOST_REQUIRE(ok);

    // The winning nonce, written LE into the trailing 4 bytes, reproduces the keys
    // the validator derives — so CuckatooVerify must accept.
    pre[pre.size() - 4] = (unsigned char)(won & 0xff);
    pre[pre.size() - 3] = (unsigned char)((won >> 8) & 0xff);
    pre[pre.size() - 2] = (unsigned char)((won >> 16) & 0xff);
    pre[pre.size() - 1] = (unsigned char)((won >> 24) & 0xff);
    const cuckatoo::Keys keys = cuckatoo::CuckatooSetHeader(pre.data(), pre.size());
    BOOST_CHECK(cuckatoo::CuckatooVerify(cyc, keys, 19));
}

BOOST_AUTO_TEST_CASE(cuckatoo_solve_bytes_28_dispatches)
{
    // EDGEBITS-28 per-tx solver (publictest/main). A real solve is memory-hard and
    // ~15 min on CPU — a reference/FALLBACK path (production grinding is GPU-class;
    // the users are GPU-running agents). Recurring a full solve here would cripple
    // the suite; the committed GOLDEN28 vector below is what proves solve/verify
    // correctness at this size. Here we only assert the E28 path links, allocates
    // its context, and routes through dispatch (max_attempts=0 → no solve) without
    // tripping the default-case assert. Solve correctness at E28 is structurally
    // identical to the E19 Bytes solver proven above.
    std::vector<unsigned char> pre(40, 0xCD);
    cuckatoo::Cycle cyc{};
    uint32_t won = 0;
    const bool found = cuckatoo::CuckatooSolveBytes(pre.data(), pre.size(), /*edgebits=*/28,
                                                    /*start=*/0, /*max=*/0, cyc, won);
    BOOST_CHECK(!found);  // 0 attempts -> no cycle; proves the 28 path links + routes
}

// Golden EDGEBITS-28 solution, captured at the shipped graph size. This is what
// makes cross-platform parity a standing property rather than a one-off event: it
// re-verifies on every build on every platform, so a solver that diverges between
// Linux and Windows fails the unit suite instead of failing in production. It also
// discharges the committed-vector debt left over from the E29 cross-platform work.
//
// Reproducible: 80-byte pre-image, ASCII "QUICKSILVER-E28-GOLDEN-91" at offset 0
// and zero elsewhere, solved at nonce 0. Bytes 76..79 are zero, which IS nonce 0
// little-endian, so these keys are exactly the keys the solver derived.
static const cuckatoo::Keys GOLDEN28_KEYS{
    0xd26332bd402ad5cdULL, 0xed5ff21818bd391fULL,
    0xd5dbfe780576de97ULL, 0x7515fc615c6d123dULL};
static const cuckatoo::Cycle GOLDEN28_CYCLE{
    0x59e882, 0xb5680f, 0xe11c12, 0x22af95d, 0x27136a0, 0x2ac7e94, 0x2b2289d,
    0x2b6d9aa, 0x3382ccd, 0x3800c2b, 0x3ba1d22, 0x4351119, 0x49c7c17, 0x4b126bc,
    0x52a22fd, 0x53f80c8, 0x554c66b, 0x5f2a485, 0x78acb89, 0x7f131bb, 0x81ca763,
    0x824c3bd, 0x830ee77, 0x8677a2a, 0x8f471a0, 0x97c9a77, 0x9e17143, 0xa38f876,
    0xa50f625, 0xb101976, 0xb67a1ff, 0xbcd6c4e, 0xc010599, 0xc027504, 0xc0f0bc1,
    0xca0600f, 0xd85a9b9, 0xde6409f, 0xe2b81a9, 0xe53508e, 0xfeca63d, 0xff0775e};

BOOST_AUTO_TEST_CASE(verify_accepts_golden28_rejects_tamper_and_wrong_size)
{
    BOOST_CHECK(cuckatoo::CuckatooVerify(GOLDEN28_CYCLE, GOLDEN28_KEYS, 28));

    cuckatoo::Cycle bad = GOLDEN28_CYCLE;
    bad[20] += 2;                       // perturb one edge, keeping the order ascending
    BOOST_CHECK(!cuckatoo::CuckatooVerify(bad, GOLDEN28_KEYS, 28));

    // A valid cycle at one size is not a cycle at another. This is what a size
    // confusion between the block and per-transaction paths would look like.
    BOOST_CHECK(!cuckatoo::CuckatooVerify(GOLDEN28_CYCLE, GOLDEN28_KEYS, 19));
    BOOST_CHECK(!cuckatoo::CuckatooVerify(GOLDEN28_CYCLE, GOLDEN28_KEYS, 29));  // 29 is not a dispatched graph size
}

//! F-253: a cycle the solver returns must be one consensus accepts.
//!
//! The vendored lean solver computes the cuckatoo check on every cycle it finds and
//! discards the verdict (its only consumer is print_log, and SQUASH_OUTPUT is 1 in both
//! solve_*.cpp), so a false cycle used to reach the caller indistinguishable from a sound
//! one and was rejected later by consensus -- after the work had been paid for.
//! CuckatooSolveBytes now self-verifies the CPU result the way it always has the GPU's.
//!
//! \warning This is a WEAK regression test, and deliberately recorded as such: before the
//! fix it fails only on a pre-image that happens to produce a false cycle, which is rare.
//! It locks in the invariant; it does not prove the bug is gone. The strong evidence is
//! structural -- SweepVerified cannot return a cycle it has not verified.
BOOST_AUTO_TEST_CASE(solve_bytes_never_returns_a_cycle_consensus_rejects)
{
    uint32_t discarded_total{0};
    for (unsigned char seed = 0; seed < 24; ++seed) {
        // Vary length as well as content: the tx pre-image is variable-length and its
        // trailing 4 bytes are the nonce slot, so alignment is part of what is exercised.
        std::vector<unsigned char> pre(33 + (seed % 7), static_cast<unsigned char>(0x40 + seed));
        cuckatoo::Cycle cyc{};
        uint32_t won{0};
        uint32_t discarded{0};
        const bool ok = cuckatoo::CuckatooSolveBytes(pre.data(), pre.size(), /*edgebits=*/19,
                                                     /*start=*/0, /*max=*/1u << 20, cyc, won,
                                                     /*cpu_fallback=*/true, {}, {},
                                                     /*gpu_status=*/nullptr, &discarded);
        BOOST_REQUIRE_MESSAGE(ok, "no cycle for seed " << int(seed));

        // Rebuild the keys exactly as consensus does, from the winning nonce.
        pre[pre.size() - 4] = static_cast<unsigned char>(won & 0xff);
        pre[pre.size() - 3] = static_cast<unsigned char>((won >> 8) & 0xff);
        pre[pre.size() - 2] = static_cast<unsigned char>((won >> 16) & 0xff);
        pre[pre.size() - 1] = static_cast<unsigned char>((won >> 24) & 0xff);
        const cuckatoo::Keys keys = cuckatoo::CuckatooSetHeader(pre.data(), pre.size());
        BOOST_CHECK_MESSAGE(cuckatoo::CuckatooVerify(cyc, keys, 19),
                            "solver returned a cycle consensus rejects, seed " << int(seed)
                            << " nonce " << won);
        discarded_total += discarded;
    }
    // Not an assertion that the count is zero -- a discard is correct behaviour, not a
    // failure. What matters is that the out-param is usable: a caller can tell a solver
    // emitting false cycles apart from one merely meeting hard graphs.
    BOOST_TEST_MESSAGE("cycles discarded across the sweep: " << discarded_total);
}

//! The counter is an out-param the caller owns; the solver must not touch it when there
//! is nothing to report, so a caller can accumulate across calls without resetting.
BOOST_AUTO_TEST_CASE(discarded_cycles_out_param_is_left_alone_on_a_clean_solve)
{
    std::vector<unsigned char> pre(37, 0xAB);
    cuckatoo::Cycle cyc{};
    uint32_t won{0};
    uint32_t discarded{7};  // a sentinel the solver has no business clearing
    BOOST_REQUIRE(cuckatoo::CuckatooSolveBytes(pre.data(), pre.size(), /*edgebits=*/19,
                                               /*start=*/0, /*max=*/1u << 20, cyc, won,
                                               /*cpu_fallback=*/true, {}, {},
                                               /*gpu_status=*/nullptr, &discarded));
    BOOST_CHECK_GE(discarded, 7u);  // only ever incremented, never reset
}

BOOST_AUTO_TEST_CASE(unsupported_edgebits_29_is_not_dispatched)
{
    std::array<unsigned char, cuckatoo::PREPOW_BYTES> prepow{};
    cuckatoo::Cycle out{};
    uint32_t nonce{0};
    BOOST_CHECK(!cuckatoo::CuckatooSolve(prepow, /*edgebits=*/29, /*start=*/0, /*max=*/0, out, nonce));
}

BOOST_AUTO_TEST_SUITE_END()
