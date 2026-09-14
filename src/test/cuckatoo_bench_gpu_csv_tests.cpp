// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Tests for the CPU-side check on GPU calibration output. The GPU harness
// (gpu/qsgpucalibrate.cu) emits the cycle it found alongside each timing; this
// re-derives the graph from the row's nonce and checks the cycle with a
// different solver family than the one that produced it.

#include <crypto/cuckatoo/bench/gpu_csv.h>
#include <crypto/cuckatoo/bench/registry.h>
#include <crypto/cuckatoo/cuckatoo.h>

#include <boost/test/unit_test.hpp>

#include <cstring>
#include <sstream>
#include <string>
#include <vector>

BOOST_AUTO_TEST_SUITE(cuckatoo_bench_gpu_csv_tests)

namespace {

const char* kHeader = "edgebits,nonce,seconds,found,device,cycle\n";

std::vector<unsigned char> CalibrationPrepow()
{
    std::vector<unsigned char> prepow(80, 0);
    std::memcpy(prepow.data(), "quicksilver-calibration-vector", 30);
    return prepow;
}

//! Solve graphs at `edgebits` until one holds a cycle; return its nonce, or -1.
int64_t FindOneCycle(unsigned edgebits, uint32_t max_graphs, cuckatoo::Cycle& out)
{
    const auto* vt = cuckatoo::bench::Lookup((uint8_t)edgebits);
    if (vt == nullptr) return -1;
    const auto prepow = CalibrationPrepow();
    void* ctx = vt->create(2);
    if (ctx == nullptr) return -1;
    int64_t winning = -1;
    for (uint32_t nonce = 0; nonce < max_graphs && winning < 0; ++nonce) {
        if (vt->solve_one(ctx, prepow.data(), prepow.size(), nonce, out)) winning = nonce;
    }
    vt->destroy(ctx);
    return winning;
}

std::string CycleField(const cuckatoo::Cycle& cyc)
{
    std::ostringstream os;
    for (int i = 0; i < cuckatoo::PROOFSIZE; ++i) {
        os << std::hex << (unsigned long long)cyc[i];
        if (i + 1 < cuckatoo::PROOFSIZE) os << ' ';
    }
    return os.str();
}

std::string Row(unsigned edgebits, uint32_t nonce, bool found, const std::string& cycle)
{
    std::ostringstream os;
    os << edgebits << ',' << nonce << ",0.5," << (found ? 1 : 0) << ",0," << cycle << '\n';
    return os.str();
}

} // namespace

// A genuine cycle, produced by the CPU solver and formatted exactly as the GPU
// harness formats one, must pass. If this fails the checker is unusable and
// every other case here is vacuous.
BOOST_AUTO_TEST_CASE(accepts_a_genuine_cycle_at_e22)
{
    cuckatoo::Cycle cyc{};
    const int64_t nonce = FindOneCycle(22, 400, cyc);
    BOOST_REQUIRE_MESSAGE(nonce >= 0, "no 42-cycle in 400 graphs at E22");

    std::istringstream in(std::string(kHeader) +
                          Row(22, (uint32_t)nonce, true, CycleField(cyc)));
    const auto v = cuckatoo::bench::VerifyGpuCsv(in, CalibrationPrepow());

    BOOST_CHECK_MESSAGE(v.error.empty(), "unexpected parse error: " << v.error);
    BOOST_CHECK_EQUAL(v.rows, 1u);
    BOOST_CHECK_EQUAL(v.found, 1u);
    BOOST_CHECK_EQUAL(v.verified, 1u);
    BOOST_CHECK(v.failed_nonces.empty());
}

// THE point of the checker: a wrong cycle must be caught. A GPU solver that
// emits garbage at a newly-built graph size is the historical failure this
// guards against, and it would otherwise be invisible in a timing sweep.
BOOST_AUTO_TEST_CASE(rejects_a_tampered_cycle)
{
    cuckatoo::Cycle cyc{};
    const int64_t nonce = FindOneCycle(22, 400, cyc);
    BOOST_REQUIRE(nonce >= 0);
    cuckatoo::Cycle bad = cyc;
    bad[cuckatoo::PROOFSIZE - 1] += 2;

    std::istringstream in(std::string(kHeader) +
                          Row(22, (uint32_t)nonce, true, CycleField(bad)));
    const auto v = cuckatoo::bench::VerifyGpuCsv(in, CalibrationPrepow());

    BOOST_CHECK(v.error.empty());
    BOOST_CHECK_EQUAL(v.verified, 0u);
    BOOST_REQUIRE_EQUAL(v.failed_nonces.size(), 1u);
    BOOST_CHECK_EQUAL(v.failed_nonces[0], (uint32_t)nonce);
}

// A cycle attributed to the wrong nonce describes a different graph. Catching
// this proves the checker re-derives the graph from the row rather than
// trusting whatever the producer attached.
BOOST_AUTO_TEST_CASE(rejects_a_cycle_attributed_to_the_wrong_nonce)
{
    cuckatoo::Cycle cyc{};
    const int64_t nonce = FindOneCycle(22, 400, cyc);
    BOOST_REQUIRE(nonce >= 0);

    std::istringstream in(std::string(kHeader) +
                          Row(22, (uint32_t)nonce + 1, true, CycleField(cyc)));
    const auto v = cuckatoo::bench::VerifyGpuCsv(in, CalibrationPrepow());

    BOOST_CHECK(v.error.empty());
    BOOST_CHECK_EQUAL(v.verified, 0u);
    BOOST_CHECK_EQUAL(v.failed_nonces.size(), 1u);
}

// At E19 the consensus verifier exists, so the GPU's output can be checked
// against the code the network actually runs — not merely against the vendored
// verifier compiled at the same size.
BOOST_AUTO_TEST_CASE(ties_to_the_consensus_verifier_at_e19)
{
    cuckatoo::Cycle cyc{};
    const int64_t nonce = FindOneCycle(19, 800, cyc);
    BOOST_REQUIRE_MESSAGE(nonce >= 0, "no 42-cycle in 800 graphs at E19");

    std::istringstream in(std::string(kHeader) +
                          Row(19, (uint32_t)nonce, true, CycleField(cyc)));
    const auto v = cuckatoo::bench::VerifyGpuCsv(in, CalibrationPrepow());

    BOOST_CHECK(v.error.empty());
    BOOST_CHECK_EQUAL(v.verified, 1u);
    BOOST_CHECK_MESSAGE(v.consensus_checked == 1u,
                        "an E19 row must be checked against CuckatooVerify, "
                        "not only against the bench verifier");
}

// A row claiming no cycle has nothing to verify, and must not be counted as if
// it had been checked.
BOOST_AUTO_TEST_CASE(counts_but_does_not_verify_rows_without_a_cycle)
{
    std::istringstream in(std::string(kHeader) + Row(22, 0, false, "") +
                          Row(22, 1, false, ""));
    const auto v = cuckatoo::bench::VerifyGpuCsv(in, CalibrationPrepow());

    BOOST_CHECK(v.error.empty());
    BOOST_CHECK_EQUAL(v.rows, 2u);
    BOOST_CHECK_EQUAL(v.found, 0u);
    BOOST_CHECK_EQUAL(v.verified, 0u);
}

// found=1 with an empty cycle field is a producer bug. Treating it as "nothing
// to check" would let a broken GPU run report a clean bill of health.
BOOST_AUTO_TEST_CASE(rejects_a_found_row_carrying_no_cycle)
{
    std::istringstream in(std::string(kHeader) + Row(22, 3, true, ""));
    const auto v = cuckatoo::bench::VerifyGpuCsv(in, CalibrationPrepow());
    BOOST_CHECK_MESSAGE(!v.error.empty(),
                        "found=1 with an empty cycle must be a hard error");
}

// A truncated proof must not be zero-padded into something that might verify.
BOOST_AUTO_TEST_CASE(rejects_a_short_proof)
{
    std::istringstream in(std::string(kHeader) + Row(22, 3, true, "1 2 3"));
    const auto v = cuckatoo::bench::VerifyGpuCsv(in, CalibrationPrepow());
    BOOST_CHECK_MESSAGE(!v.error.empty(), "a proof of 3 indices must be an error");
}

// A size with no bench solver cannot be checked at all. Reporting success would
// mean "unverified" and "verified" look identical downstream.
BOOST_AUTO_TEST_CASE(rejects_an_unregistered_edgebits)
{
    std::istringstream in(std::string(kHeader) + Row(31, 0, true, "1 2 3"));
    const auto v = cuckatoo::bench::VerifyGpuCsv(in, CalibrationPrepow());
    BOOST_CHECK_MESSAGE(!v.error.empty(),
                        "edgebits with no registered bench solver must be an error");
}

// A file whose header does not match the contract is not this format, and its
// columns may mean something else entirely.
BOOST_AUTO_TEST_CASE(rejects_a_foreign_header)
{
    // This is qscalibrate's CPU schema: same first four columns, no cycle.
    std::istringstream in("edgebits,nonce,seconds,found,threads\n22,0,0.5,0,8\n");
    const auto v = cuckatoo::bench::VerifyGpuCsv(in, CalibrationPrepow());
    BOOST_CHECK_MESSAGE(!v.error.empty(),
                        "the CPU sweep's schema must not be accepted as GPU output");
}

BOOST_AUTO_TEST_SUITE_END()
