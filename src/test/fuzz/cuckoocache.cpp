// Copyright (c) 2020-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cuckoocache.h>
#include <script/sigcache.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <test/util/setup_common.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace {
std::array<uint32_t, 8> g_hash_salts{};

struct StableHasher {
    template <uint8_t N>
    uint32_t operator()(const int& value) const
    {
        return g_hash_salts[N] ^ (value == 1 ? 0x9e3779b9U : 0x85ebca6bU);
    }
};
} // namespace

FUZZ_TARGET(cuckoocache)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());
    for (auto& salt : g_hash_salts) salt = fuzzed_data_provider.ConsumeIntegral<uint32_t>();
    CuckooCache::cache<int, StableHasher> cuckoo_cache{};
    if (fuzzed_data_provider.ConsumeBool()) {
        const size_t megabytes = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, 16);
        cuckoo_cache.setup_bytes(megabytes << 20);
    } else {
        cuckoo_cache.setup(fuzzed_data_provider.ConsumeIntegralInRange<uint32_t>(0, 4096));
    }
    LIMITED_WHILE(fuzzed_data_provider.ConsumeBool(), 10000) {
        if (fuzzed_data_provider.ConsumeBool()) {
            const int value = fuzzed_data_provider.ConsumeBool() ? 1 : 2;
            cuckoo_cache.insert(value);
            assert(cuckoo_cache.contains(value, false));
        } else {
            const int e = fuzzed_data_provider.ConsumeBool() ? 1 : 2;
            auto erase = fuzzed_data_provider.ConsumeBool();
            const bool before = cuckoo_cache.contains(e, false);
            assert(cuckoo_cache.contains(e, erase) == before);
        }
    }
}
