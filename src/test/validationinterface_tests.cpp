// Copyright (c) 2020 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>
#include <consensus/validation.h>
#include <primitives/block.h>
#include <scheduler.h>
#include <test/util/setup_common.h>
#include <util/check.h>
#include <kernel/chain.h>
#include <validationinterface.h>

#include <atomic>
#include <chrono>
#include <thread>

BOOST_FIXTURE_TEST_SUITE(validationinterface_tests, ChainTestingSetup)

struct TestSubscriberNoop final : public CValidationInterface {
    void BlockChecked(const CBlock&, const BlockValidationState&) override {}
};

struct TestSubscriberCounting final : public CValidationInterface {
    std::atomic<uint64_t> calls{0};
    void BlockChecked(const CBlock&, const BlockValidationState&) override { ++calls; }
};

BOOST_AUTO_TEST_CASE(unregister_validation_interface_race)
{
    std::atomic<bool> generate{true};
    // A subscriber that stays registered for the whole test, so every
    // dispatch the generator makes is observable (S0382-B-01).
    auto counter = std::make_shared<TestSubscriberCounting>();
    m_node.validation_signals->RegisterSharedValidationInterface(counter);

    // Start thread to generate notifications
    std::thread gen{[&] {
        const CBlock block_dummy;
        BlockValidationState state_dummy;
        while (generate) {
            m_node.validation_signals->BlockChecked(block_dummy, state_dummy);
        }
    }};

    // Start thread to consume notifications
    uint64_t calls_before{0};
    uint64_t calls_after{0};
    bool generator_started{false};
    std::thread sub{[&] {
        // Do not start the register/unregister loop until the generator has
        // really delivered a notification; otherwise the loop can run to the
        // end before the generator is ever scheduled and nothing races.
        const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds{30}};
        while (counter->calls.load() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        generator_started = counter->calls.load() > 0;
        calls_before = counter->calls.load();
        // keep going for about 1 sec, which is 250k iterations
        for (int i = 0; generator_started && i < 250000; i++) {
            auto sub = std::make_shared<TestSubscriberNoop>();
            m_node.validation_signals->RegisterSharedValidationInterface(sub);
            m_node.validation_signals->UnregisterSharedValidationInterface(sub);
        }
        calls_after = counter->calls.load();
        // tell the other thread we are done
        generate = false;
    }};

    gen.join();
    sub.join();
    m_node.validation_signals->UnregisterSharedValidationInterface(counter);
    BOOST_CHECK(!generate);
    BOOST_REQUIRE_MESSAGE(generator_started, "generator delivered no BlockChecked within 30 s");
    // Dispatch kept running while subscribers were registered and removed.
    // The bound is one call, not a rate: a loaded CI runner may deschedule the
    // generator for most of the loop, and a rate would make the test flaky.
    BOOST_TEST_MESSAGE("BlockChecked calls during the registration loop: " << (calls_after - calls_before));
    BOOST_CHECK_GT(calls_after - calls_before, 0U);
}

class TestInterface : public CValidationInterface
{
public:
    TestInterface(ValidationSignals& signals, std::function<void()> on_call = nullptr, std::function<void()> on_destroy = nullptr)
        : m_on_call(std::move(on_call)), m_on_destroy(std::move(on_destroy)), m_signals{signals}
    {
    }
    virtual ~TestInterface()
    {
        if (m_on_destroy) m_on_destroy();
    }
    void BlockChecked(const CBlock& block, const BlockValidationState& state) override
    {
        if (m_on_call) m_on_call();
    }
    void Call()
    {
        CBlock block;
        BlockValidationState state;
        m_signals.BlockChecked(block, state);
    }
    std::function<void()> m_on_call;
    std::function<void()> m_on_destroy;
    ValidationSignals& m_signals;
};

// Regression test to ensure UnregisterAllValidationInterfaces calls don't
// destroy a validation interface while it is being called.
BOOST_AUTO_TEST_CASE(unregister_all_during_call)
{
    bool destroyed = false;
    auto shared{std::make_shared<TestInterface>(
        *m_node.validation_signals,
        [&] {
            // First call should decrements reference count 2 -> 1
            m_node.validation_signals->UnregisterAllValidationInterfaces();
            BOOST_CHECK(!destroyed);
            // Second call should not decrement reference count 1 -> 0
            m_node.validation_signals->UnregisterAllValidationInterfaces();
            BOOST_CHECK(!destroyed);
        },
        [&] { destroyed = true; })};
    m_node.validation_signals->RegisterSharedValidationInterface(shared);
    BOOST_CHECK(shared.use_count() == 2);
    shared->Call();
    BOOST_CHECK(shared.use_count() == 1);
    BOOST_CHECK(!destroyed);
    shared.reset();
    BOOST_CHECK(destroyed);
}

BOOST_AUTO_TEST_SUITE_END()
