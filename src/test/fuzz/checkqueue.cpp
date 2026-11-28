// Copyright (c) 2020-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <checkqueue.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {
struct DumbCheck {
    bool result = false;

    explicit DumbCheck(const bool _result) : result(_result)
    {
    }

    std::optional<int> operator()() const
    {
        if (result) return std::nullopt;
        return 1;
    }
};
} // namespace

FUZZ_TARGET(checkqueue)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());

    const unsigned int batch_size = fuzzed_data_provider.ConsumeIntegralInRange<unsigned int>(0, 1024);
    CCheckQueue<DumbCheck> check_queue_1{batch_size, /*worker_threads_num=*/0};
    CCheckQueue<DumbCheck> check_queue_2{batch_size, /*worker_threads_num=*/0};
    std::vector<DumbCheck> checks_1;
    std::vector<DumbCheck> checks_2;
    const int size = fuzzed_data_provider.ConsumeIntegralInRange<int>(0, 1024);
    for (int i = 0; i < size; ++i) {
        const bool result = fuzzed_data_provider.ConsumeBool();
        checks_1.emplace_back(result);
        checks_2.emplace_back(result);
    }
    // S0392-B-02: with zero workers the master thread runs every check in
    // Complete(), so its result is asserted: the first failing check's value
    // (1) if any added check fails, otherwise nullopt. When Complete() is not
    // called, the case only covers teardown with checks still queued.
    const bool any_failure{std::any_of(checks_1.begin(), checks_1.end(), [](const DumbCheck& c) { return !c.result; })};
    bool added_1{false};
    if (fuzzed_data_provider.ConsumeBool()) {
        check_queue_1.Add(std::move(checks_1));
        added_1 = true;
    }
    if (fuzzed_data_provider.ConsumeBool()) {
        const std::optional<int> result{check_queue_1.Complete()};
        assert(result == ((added_1 && any_failure) ? std::optional<int>{1} : std::nullopt));
    }

    CCheckQueueControl<DumbCheck> check_queue_control{&check_queue_2};
    bool added_2{false};
    if (fuzzed_data_provider.ConsumeBool()) {
        check_queue_control.Add(std::move(checks_2));
        added_2 = true;
    }
    if (fuzzed_data_provider.ConsumeBool()) {
        const std::optional<int> result{check_queue_control.Complete()};
        assert(result == ((added_2 && any_failure) ? std::optional<int>{1} : std::nullopt));
    }
}
