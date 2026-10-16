// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <crypto/cuckatoo/cuckatoo.h>
#include <util/process/contained_child.h>
#include <boost/test/unit_test.hpp>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace cuckatoo { bool Solve19Bytes(const unsigned char*, size_t, uint32_t, uint32_t, Cycle&, uint32_t&, const SolverProgressCallback&, const SolverCancelCallback&); }
namespace {
boost::test_tools::assertion_result opencl_device_named(boost::unit_test::test_unit_id)
{
    const char* d = std::getenv("QS_OPENCL_TEST_DEVICE");
    if (d && d[0]) return true;
    boost::test_tools::assertion_result ans(false); ans.message() << "QS_OPENCL_TEST_DEVICE unset — skipping OpenCL parity gate"; return ans;
}
std::string Hex(const std::vector<unsigned char>& v) { static const char* h = "0123456789abcdef"; std::string s; for (unsigned char b : v) { s += h[b >> 4]; s += h[b & 15]; } return s; }
std::string Dec(uint32_t v) { char b[16]; std::snprintf(b, sizeof(b), "%u", v); return b; }
bool ParseSolution(const std::string& out, uint32_t& nonce, cuckatoo::Cycle& cyc)
{
    const size_t n = out.find("nonce="), c = out.find("cycle=");
    if (n == std::string::npos || c == std::string::npos) return false;
    nonce = static_cast<uint32_t>(std::strtoul(out.c_str() + n + 6, nullptr, 10));
    const char* p = out.c_str() + c + 6;
    for (int i = 0; i < cuckatoo::PROOFSIZE; ++i) { char* end{nullptr}; cyc[i] = static_cast<uint32_t>(std::strtoull(p, &end, 16)); if (end == p) return false; p = end; }
    return true;
}
} // namespace

BOOST_AUTO_TEST_SUITE(opencl_parity_tests)
BOOST_AUTO_TEST_CASE(matches_cpu_solver_e19, *boost::unit_test::precondition(opencl_device_named))
{
    const std::string device = std::getenv("QS_OPENCL_TEST_DEVICE"); constexpr uint32_t kWindow{200}; int cycles_found{0};
    for (int seed = 0; seed < 8; ++seed) {
        std::vector<unsigned char> pre(84); for (size_t i = 0; i < pre.size(); ++i) pre[i] = static_cast<unsigned char>(seed * 31 + i);
        cuckatoo::Cycle cpu{}; uint32_t cpu_nonce{0};
        const bool cpu_found = cuckatoo::Solve19Bytes(pre.data(), pre.size(), 0, kWindow, cpu, cpu_nonce, {}, {});
        auto child = util::ContainedChild::Spawn({QSGPUSOLVE_OPENCL_E19_PATH, "--device", device, "19", Hex(pre), "0", Dec(kWindow)}, util::ChildStdout::kPipe);
        BOOST_REQUIRE(child); std::string out; char buf[4096];
        for (;;) { size_t got{0}; const auto st = child->ReadStdout(buf, sizeof(buf), 120000, got); if (st != util::ContainedChild::ReadStatus::kData) break; out.append(buf, got); }
        BOOST_REQUIRE_EQUAL(child->Wait(), 0);
        cuckatoo::Cycle gpu{}; uint32_t gpu_nonce{0}; const bool gpu_found = ParseSolution(out, gpu_nonce, gpu);
        BOOST_CHECK_EQUAL(gpu_found, cpu_found);
        if (cpu_found && gpu_found) {
            ++cycles_found; BOOST_CHECK_EQUAL(gpu_nonce, cpu_nonce); BOOST_CHECK_EQUAL_COLLECTIONS(gpu.begin(), gpu.end(), cpu.begin(), cpu.end());
            std::vector<unsigned char> keyed = pre; keyed[80] = gpu_nonce & 0xff; keyed[81] = (gpu_nonce >> 8) & 0xff; keyed[82] = (gpu_nonce >> 16) & 0xff; keyed[83] = (gpu_nonce >> 24) & 0xff;
            BOOST_CHECK(cuckatoo::CuckatooVerify(gpu, cuckatoo::CuckatooSetHeader(keyed.data(), static_cast<uint32_t>(keyed.size())), 19));
        }
    }
    BOOST_CHECK_GT(cycles_found, 0);
}
BOOST_AUTO_TEST_SUITE_END()
