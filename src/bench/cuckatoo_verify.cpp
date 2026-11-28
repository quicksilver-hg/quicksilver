// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <crypto/cuckatoo/cuckatoo.h>
#include <uint256.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>

namespace {
// The golden EDGEBITS-28 vector, copied from src/test/cuckatoo_tests.cpp
// (GOLDEN28_KEYS, GOLDEN28_CYCLE): the 80-byte pre-image is ASCII
// "QUICKSILVER-E28-GOLDEN-91" at offset 0 and zero elsewhere, solved at nonce 0.
// No solve and no GPU: these benchmarks time verification and hashing only.
constexpr cuckatoo::Keys GOLDEN28_KEYS{
    0xd26332bd402ad5cdULL, 0xed5ff21818bd391fULL,
    0xd5dbfe780576de97ULL, 0x7515fc615c6d123dULL};
constexpr cuckatoo::Cycle GOLDEN28_CYCLE{
    0x59e882, 0xb5680f, 0xe11c12, 0x22af95d, 0x27136a0, 0x2ac7e94, 0x2b2289d,
    0x2b6d9aa, 0x3382ccd, 0x3800c2b, 0x3ba1d22, 0x4351119, 0x49c7c17, 0x4b126bc,
    0x52a22fd, 0x53f80c8, 0x554c66b, 0x5f2a485, 0x78acb89, 0x7f131bb, 0x81ca763,
    0x824c3bd, 0x830ee77, 0x8677a2a, 0x8f471a0, 0x97c9a77, 0x9e17143, 0xa38f876,
    0xa50f625, 0xb101976, 0xb67a1ff, 0xbcd6c4e, 0xc010599, 0xc027504, 0xc0f0bc1,
    0xca0600f, 0xd85a9b9, 0xde6409f, 0xe2b81a9, 0xe53508e, 0xfeca63d, 0xff0775e};

std::array<unsigned char, 80> Golden28PreImage()
{
    std::array<unsigned char, 80> pre{};
    const char tag[]{"QUICKSILVER-E28-GOLDEN-91"};
    std::memcpy(pre.data(), tag, sizeof(tag) - 1);
    return pre;
}

bool SameKeys(const cuckatoo::Keys& a, const cuckatoo::Keys& b)
{
    return a.k0 == b.k0 && a.k1 == b.k1 && a.k2 == b.k2 && a.k3 == b.k3;
}
} // namespace

// S0303-B-02 (verify half): one 42-cycle verification at the shipped graph size.
static void CuckatooVerifyCycleE28(benchmark::Bench& bench)
{
    // Success path asserted before timing; a tampered cycle is rejected.
    assert(cuckatoo::CuckatooVerify(GOLDEN28_CYCLE, GOLDEN28_KEYS, 28));
    cuckatoo::Cycle tampered{GOLDEN28_CYCLE};
    tampered[41] ^= 1;
    assert(!cuckatoo::CuckatooVerify(tampered, GOLDEN28_KEYS, 28));

    bench.run([&] {
        const bool ok{cuckatoo::CuckatooVerify(GOLDEN28_CYCLE, GOLDEN28_KEYS, 28)};
        assert(ok);
        ankerl::nanobench::doNotOptimizeAway(ok);
    });
}

// S0304-C-02: the BLAKE2b header-key derivation.
static void CuckatooSetHeaderBlake2b(benchmark::Bench& bench)
{
    const auto pre{Golden28PreImage()};
    assert(SameKeys(cuckatoo::CuckatooSetHeader(pre.data(), pre.size()), GOLDEN28_KEYS));

    bench.run([&] {
        const cuckatoo::Keys keys{cuckatoo::CuckatooSetHeader(pre.data(), pre.size())};
        ankerl::nanobench::doNotOptimizeAway(keys);
    });
}

// S0304-C-02: the BLAKE2b proof hash over a cycle (the value nBits thresholds).
static void CuckatooProofHashCycle(benchmark::Bench& bench)
{
    const uint256 expected{cuckatoo::CuckatooProofHash(GOLDEN28_CYCLE)};
    assert(!expected.IsNull());

    bench.run([&] {
        const uint256 hash{cuckatoo::CuckatooProofHash(GOLDEN28_CYCLE)};
        assert(hash == expected);
        ankerl::nanobench::doNotOptimizeAway(hash);
    });
}

BENCHMARK(CuckatooVerifyCycleE28, benchmark::PriorityLevel::HIGH);
BENCHMARK(CuckatooSetHeaderBlake2b, benchmark::PriorityLevel::HIGH);
BENCHMARK(CuckatooProofHashCycle, benchmark::PriorityLevel::HIGH);
