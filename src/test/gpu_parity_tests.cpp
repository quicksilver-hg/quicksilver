// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// GATE: prove GPU-produced E28 cycles verify under the node's consensus
// CuckatooVerify, for BOTH PoW paths: per-tx variable-length pre-images and
// fixed-width (84-byte) block headers. Runs only when
// CUCKATOO_GPU_SOLVER points at a qsgpusolve binary. Otherwise the cases
// skip -- Boost prints "is skipped because", and this suite's ctest row
// matches SKIP_REGULAR_EXPRESSION -- so a GPU-less machine reports a skip
// rather than a silent pass.
#include <crypto/cuckatoo/cuckatoo.h>
#include <crypto/cuckatoo/gpu_solver.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstdlib>
#include <vector>

BOOST_AUTO_TEST_SUITE(gpu_parity_tests)

namespace {
// Build a representative variable-length per-tx preimage (NOT a block pre-pow): 96 bytes,
// deterministic, last 4 bytes are the nonce slot (overwritten by the solver).
std::vector<unsigned char> make_preimage() {
    std::vector<unsigned char> p(96);
    for (size_t i = 0; i < p.size(); ++i) p[i] = (unsigned char)(0xA0 + (i & 0x0f));
    return p;
}
// 84-byte block-style pre-pow (the block PoW path), last 4 bytes are the nonce
// slot (overwritten by the solver).
std::vector<unsigned char> make_block_preimage() {
    std::vector<unsigned char> p(84);
    for (size_t i = 0; i < p.size(); ++i) p[i] = (unsigned char)(0x30 + (i & 0x1f));
    return p;
}

// Boost 1.83 has no BOOST_TEST_SKIP. A failing precondition is the documented
// runtime skip: the case is marked skipped and the reason is logged.
boost::test_tools::assertion_result gpu_solver_configured(boost::unit_test::test_unit_id)
{
    if (std::getenv("CUCKATOO_GPU_SOLVER")) {
        return true;
    }
    boost::test_tools::assertion_result ans(false);
    ans.message() << "CUCKATOO_GPU_SOLVER unset — skipping GPU parity gate";
    return ans;
}
} // namespace

BOOST_AUTO_TEST_CASE(verifies, *boost::unit_test::precondition(gpu_solver_configured))
{
    auto pre = make_preimage();

    // Drive the real bridge (watchdog + parse) exactly as the vault/miner do.
    cuckatoo::Cycle cyc{}; uint32_t nonce = 0;
    BOOST_REQUIRE_MESSAGE(
        cuckatoo::GpuSolveBytes(pre.data(), pre.size(), 28, 1, 100000, cyc, nonce),
        "qsgpusolve produced no cycle in window — raise max_attempts");

    // Reconstruct keys from the SAME bytes the solver hashed: nonce at buf[len-4..].
    pre[pre.size() - 4] = (unsigned char)(nonce & 0xff);
    pre[pre.size() - 3] = (unsigned char)((nonce >> 8) & 0xff);
    pre[pre.size() - 2] = (unsigned char)((nonce >> 16) & 0xff);
    pre[pre.size() - 1] = (unsigned char)((nonce >> 24) & 0xff);
    cuckatoo::Keys keys = cuckatoo::CuckatooSetHeader(pre.data(), (uint32_t)pre.size());

    // GATE: genuine GPU proof must verify under consensus.
    BOOST_CHECK(cuckatoo::CuckatooVerify(cyc, keys, 28));

    // Tamper one index — must be rejected.
    cuckatoo::Cycle bad = cyc;
    bad[cuckatoo::PROOFSIZE - 1] += 2;
    BOOST_CHECK(!cuckatoo::CuckatooVerify(bad, keys, 28));
}

BOOST_AUTO_TEST_CASE(verifies_e28_block, *boost::unit_test::precondition(gpu_solver_configured))
{
    auto pre = make_block_preimage();

    // Drive the real bridge exactly as the block miner does (84-byte header, E28).
    cuckatoo::Cycle cyc{}; uint32_t nonce = 0;
    BOOST_REQUIRE_MESSAGE(
        cuckatoo::GpuSolveBytes(pre.data(), pre.size(), 28, 1, 100000, cyc, nonce),
        "qsgpusolve (E28) produced no cycle for the 84-byte block header — raise max_attempts");

    // Reconstruct keys from the SAME bytes the solver hashed: nonce at buf[len-4..].
    pre[pre.size() - 4] = (unsigned char)(nonce & 0xff);
    pre[pre.size() - 3] = (unsigned char)((nonce >> 8) & 0xff);
    pre[pre.size() - 2] = (unsigned char)((nonce >> 16) & 0xff);
    pre[pre.size() - 1] = (unsigned char)((nonce >> 24) & 0xff);
    cuckatoo::Keys keys = cuckatoo::CuckatooSetHeader(pre.data(), (uint32_t)pre.size());

    // GATE: genuine GPU proof must verify under consensus at the block graph size.
    BOOST_CHECK(cuckatoo::CuckatooVerify(cyc, keys, 28));

    // Tamper one index — must be rejected.
    cuckatoo::Cycle bad = cyc;
    bad[cuckatoo::PROOFSIZE - 1] += 2;
    BOOST_CHECK(!cuckatoo::CuckatooVerify(bad, keys, 28));
}

BOOST_AUTO_TEST_SUITE_END()
