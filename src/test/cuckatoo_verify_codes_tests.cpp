// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// F-255: every vendored verify() rejection code is a named, biting test.
//
// CuckatooVerify is bool, so production negatives only see false. Existing
// keyed negatives (cuckatoo_tests.cpp, pow_tests.cpp) die at the XOR
// fast-reject (POW_NON_MATCHING). POW_BRANCH, POW_DEAD_END, and POW_SHORT_CYCLE
// are the walk's own outcomes and are unreachable with real keys at practical
// cost (~2^-38 of random E19 proofs pass XOR).
//
// Design: compile the same vendor/cuckatoo.h *source text* as Verify19 inside a
// test-only namespace whose siphash_keys is a table, not SipHash. Ordinary
// name lookup binds sipnode() to that fake. A second namespace includes the
// header without a fake and is the real keyed verifier. Zero production files
// change.
//
// Honest limit: the fake-key instantiation compiles the same source text as
// production Verify19, not the same object code. Real-key assertions cover the
// seam; the fake-key cases cover the walk logic. A construction that is POW_OK
// only under the fake table (and POW_NON_MATCHING under real GOLDEN19 keys)
// is the check that the fake, not SipHash, is what ran.

#include <crypto/cuckatoo/vendor_prelude.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstdint>

// Duplicated from src/test/cuckatoo_tests.cpp (GOLDEN19_KEYS / GOLDEN19_CYCLE).
// A shared header would be a third tracked file; this slice may only add this
// TU, one CMake line, and the lint allowlist.
static constexpr uint64_t GOLDEN19_K0{0xd23109bd4dac0bdfULL};
static constexpr uint64_t GOLDEN19_K1{0x76fbe03c31ad8133ULL};
static constexpr uint64_t GOLDEN19_K2{0xd17f301a7a865e22ULL};
static constexpr uint64_t GOLDEN19_K3{0xbaf57c804957a342ULL};
static constexpr uint32_t GOLDEN19_CYCLE[42]{
    0x19ce, 0xc1c9, 0xe95f, 0xfc42, 0x1034f, 0x1192c, 0x129a4, 0x12a44,
    0x12a58, 0x178c4, 0x1b393, 0x1eeb6, 0x1f047, 0x23272, 0x2449b, 0x25635,
    0x263ca, 0x2b2ce, 0x2db7e, 0x2f224, 0x3054d, 0x3468d, 0x374c4, 0x3c4e4,
    0x451e1, 0x47cbb, 0x4c7d3, 0x53b59, 0x555af, 0x560c4, 0x5a67c, 0x62c04,
    0x65f3d, 0x695e0, 0x6fa68, 0x77639, 0x7a06b, 0x7b2f2, 0x7cba1, 0x7d03e,
    0x7d5b2, 0x7f26b};

#define EDGEBITS 19
#define PROOFSIZE 42
#define SQUASH_OUTPUT 1
namespace verify_codes_fake {
struct siphash_keys {
    std::array<uint64_t, 84> endpoint{};
    mutable uint32_t calls{0};
    uint64_t siphash24(uint64_t nonce) const
    {
        ++calls;
        if (nonce >= endpoint.size()) return 0;
        return endpoint[static_cast<size_t>(nonce)];
    }
    void setkeys(const char*) {}
};
#include <crypto/cuckatoo/vendor/cuckatoo.h>  // fake-key siphash_keys
}
#undef EDGEBITS
#undef PROOFSIZE
#undef SQUASH_OUTPUT

#define EDGEBITS 19
#define PROOFSIZE 42
#define SQUASH_OUTPUT 1
namespace verify_codes_real {
#include <crypto/cuckatoo/vendor/cuckatoo.h>  // real ::siphash_keys
}
#undef EDGEBITS
#undef PROOFSIZE
#undef SQUASH_OUTPUT
#undef SIZEMASK
#undef NNODES1
#undef NODEMASK
#undef NODE1MASK
#undef EDGEMASK
#undef NEDGES
#undef MAX_SOLS
#undef MAX_NAME_LEN
#undef C_CALL_CONVENTION
#undef CALL_CONVENTION

BOOST_AUTO_TEST_SUITE(cuckatoo_verify_codes_tests)

namespace {

constexpr int kProof = 42;
constexpr int kEnds = 84;

void WritePairCycle(uint32_t* uvs, int edge0, int len, int pair_base)
{
    const int np = len / 2;
    for (int k = 0; k < len; ++k) {
        const int e = edge0 + k;
        const int u_pair = pair_base + (k / 2) % np;
        const int u_bit = k & 1;
        const int v_pair = pair_base + ((k & 1) ? ((k / 2 + 1) % np) : (k / 2));
        const int v_bit = k & 1;
        uvs[2 * e] = (static_cast<uint32_t>(u_pair) << 1) | static_cast<uint32_t>(u_bit);
        uvs[2 * e + 1] = (static_cast<uint32_t>(v_pair) << 1) | static_cast<uint32_t>(v_bit);
    }
}

int FakeVerify(const uint32_t* uvs, uint32_t* calls = nullptr)
{
    verify_codes_fake::siphash_keys keys{};
    std::array<verify_codes_fake::word_t, 42> edges{};
    for (int i = 0; i < kProof; ++i) {
        edges[static_cast<size_t>(i)] = static_cast<verify_codes_fake::word_t>(i);
        keys.endpoint[static_cast<size_t>(2 * i)] = uvs[2 * i];
        keys.endpoint[static_cast<size_t>(2 * i + 1)] = uvs[2 * i + 1];
    }
    const int code = verify_codes_fake::verify(edges.data(), &keys);
    if (calls) *calls = keys.calls;
    return code;
}

::siphash_keys Golden19SipKeys()
{
    ::siphash_keys sk;
    sk.k0 = GOLDEN19_K0;
    sk.k1 = GOLDEN19_K1;
    sk.k2 = GOLDEN19_K2;
    sk.k3 = GOLDEN19_K3;
    return sk;
}

int RealVerify(const uint32_t* cycle)
{
    ::siphash_keys sk = Golden19SipKeys();
    std::array<verify_codes_real::word_t, 42> edges{};
    for (int i = 0; i < kProof; ++i) {
        edges[static_cast<size_t>(i)] = cycle[i];
    }
    return verify_codes_real::verify(edges.data(), &sk);
}

} // namespace

BOOST_AUTO_TEST_CASE(real_golden19_is_pow_ok)
{
    BOOST_CHECK_EQUAL(RealVerify(GOLDEN19_CYCLE), verify_codes_real::POW_OK);
}

BOOST_AUTO_TEST_CASE(fake_node_pair_cycle_is_pow_ok)
{
    uint32_t uvs[kEnds];
    WritePairCycle(uvs, 0, kProof, 0);
    uint32_t calls = 0;
    BOOST_CHECK_EQUAL(FakeVerify(uvs, &calls), verify_codes_fake::POW_OK);
    BOOST_CHECK_EQUAL(calls, 84u); // 42 edges × 2 sipnode lookups
}

BOOST_AUTO_TEST_CASE(fake_is_bound_not_siphash)
{
    // POW_OK under the fake table and POW_NON_MATCHING under real keys on the
    // same edge list (0..41) means sipnode() used the namespace-local fake.
    uint32_t uvs[kEnds];
    WritePairCycle(uvs, 0, kProof, 0);
    BOOST_CHECK_EQUAL(FakeVerify(uvs), verify_codes_fake::POW_OK);

    uint32_t edges[42];
    for (int i = 0; i < kProof; ++i) edges[i] = static_cast<uint32_t>(i);
    BOOST_CHECK_EQUAL(RealVerify(edges), verify_codes_real::POW_NON_MATCHING);
}

BOOST_AUTO_TEST_CASE(real_too_big)
{
    uint32_t cycle[42];
    for (int i = 0; i < kProof; ++i) cycle[i] = GOLDEN19_CYCLE[i];
    cycle[41] = 0x80000; // NODEMASK+1 at EDGEBITS 19
    BOOST_CHECK_EQUAL(RealVerify(cycle), verify_codes_real::POW_TOO_BIG);
}

BOOST_AUTO_TEST_CASE(real_too_small_swapped)
{
    uint32_t cycle[42];
    for (int i = 0; i < kProof; ++i) cycle[i] = GOLDEN19_CYCLE[i];
    const uint32_t tmp = cycle[40];
    cycle[40] = cycle[41];
    cycle[41] = tmp;
    BOOST_CHECK_EQUAL(RealVerify(cycle), verify_codes_real::POW_TOO_SMALL);
}

BOOST_AUTO_TEST_CASE(real_too_small_duplicate)
{
    uint32_t cycle[42];
    for (int i = 0; i < kProof; ++i) cycle[i] = GOLDEN19_CYCLE[i];
    cycle[41] = cycle[40];
    BOOST_CHECK_EQUAL(RealVerify(cycle), verify_codes_real::POW_TOO_SMALL);
}

BOOST_AUTO_TEST_CASE(real_non_matching_tamper)
{
    uint32_t cycle[42];
    for (int i = 0; i < kProof; ++i) cycle[i] = GOLDEN19_CYCLE[i];
    cycle[20] += 2; // stays strictly increasing
    BOOST_CHECK_EQUAL(RealVerify(cycle), verify_codes_real::POW_NON_MATCHING);
}

BOOST_AUTO_TEST_CASE(real_non_matching_edges_0_41)
{
    uint32_t cycle[42];
    for (int i = 0; i < kProof; ++i) cycle[i] = static_cast<uint32_t>(i);
    BOOST_CHECK_EQUAL(RealVerify(cycle), verify_codes_real::POW_NON_MATCHING);
}

BOOST_AUTO_TEST_CASE(fake_branch_xor_balanced)
{
    // Third U-endpoint on pair 0. XOR is restored by flipping the same delta
    // into a later U the walk never reaches (edge 20). Real verify() cannot
    // skip XOR, so an unbalanced graph would report POW_NON_MATCHING instead.
    uint32_t uvs[kEnds];
    WritePairCycle(uvs, 0, kProof, 0);
    const uint32_t orig = uvs[4];
    uvs[4] = (uvs[0] >> 1) << 1;
    uvs[40] ^= orig ^ uvs[4];
    BOOST_CHECK_EQUAL(FakeVerify(uvs), verify_codes_fake::POW_BRANCH);
}

BOOST_AUTO_TEST_CASE(fake_dead_end_no_partner_xor_balanced)
{
    // Pair 0 has a single U-endpoint (j == i). Partner is moved to a fresh
    // pair id; XOR restored on edge 20's U.
    uint32_t uvs[kEnds];
    WritePairCycle(uvs, 0, kProof, 0);
    const uint32_t orig = uvs[2];
    uvs[2] = 50u << 1;
    uvs[40] ^= orig ^ uvs[2];
    BOOST_CHECK_EQUAL(FakeVerify(uvs), verify_codes_fake::POW_DEAD_END);
}

BOOST_AUTO_TEST_CASE(fake_dead_end_same_g_node_xor_balanced)
{
    // uvs[j] == uvs[i]: both U-endpoints of pair 0 share LSB 0. Pair 1 is
    // given the same treatment so the XOR of U-endpoints is unchanged
    // (each matching pair contributes 1; two broken pairs contribute 0^0).
    uint32_t uvs[kEnds];
    WritePairCycle(uvs, 0, kProof, 0);
    uvs[2] = uvs[0];
    uvs[6] = uvs[4];
    BOOST_CHECK_EQUAL(FakeVerify(uvs), verify_codes_fake::POW_DEAD_END);
}

BOOST_AUTO_TEST_CASE(fake_short_cycle_12_plus_30_xor_balanced)
{
    // Slice-3 construction: 12-cycle on edges 0..11, 30-cycle on 12..41.
    // Already XOR-balanced (6 even + 15 odd U-pairs).
    uint32_t uvs[kEnds];
    WritePairCycle(uvs, 0, 12, 0);
    WritePairCycle(uvs, 12, 30, 100);
    BOOST_CHECK_EQUAL(FakeVerify(uvs), verify_codes_fake::POW_SHORT_CYCLE);
}

BOOST_AUTO_TEST_CASE(fake_bucket_collision_pair_64_still_ok)
{
    // Pair ids 0 and 64 share (>>1) & SIZEMASK (== 0). The match test uses
    // the full pair id, so a colliding occupant is skipped and the cycle
    // still verifies. This is the path most likely to be "optimised" wrongly.
    uint32_t uvs[kEnds];
    WritePairCycle(uvs, 0, kProof, 0);
    for (int k = 0; k < kEnds; ++k) {
        if ((uvs[k] >> 1) == 1u) {
            uvs[k] = (64u << 1) | (uvs[k] & 1u);
        }
    }
    BOOST_CHECK_EQUAL(FakeVerify(uvs), verify_codes_fake::POW_OK);
}

BOOST_AUTO_TEST_CASE(fake_bucket_all_u_in_one_bucket_still_ok)
{
    // Every U-endpoint's pair id is a multiple of 64, so all land in bucket 0.
    // Inner walk visits 41 others and still returns POW_OK.
    uint32_t uvs[kEnds];
    WritePairCycle(uvs, 0, kProof, 0);
    for (int k = 0; k < kProof; ++k) {
        const int np = kProof / 2;
        const int u_pair = (k / 2) % np;
        const int u_bit = k & 1;
        const int v_pair = (k & 1) ? ((k / 2 + 1) % np) : (k / 2);
        const int v_bit = k & 1;
        uvs[2 * k] = (static_cast<uint32_t>(u_pair) * 64u << 1) | static_cast<uint32_t>(u_bit);
        uvs[2 * k + 1] = (static_cast<uint32_t>(v_pair) * 64u << 1) | static_cast<uint32_t>(v_bit);
    }
    BOOST_CHECK_EQUAL(FakeVerify(uvs), verify_codes_fake::POW_OK);
}

BOOST_AUTO_TEST_SUITE_END()
