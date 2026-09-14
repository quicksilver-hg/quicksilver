// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <crypto/cuckatoo/bench/registry.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>

BOOST_AUTO_TEST_SUITE(cuckatoo_bench_registry_tests)

BOOST_AUTO_TEST_CASE(lookup_returns_null_for_unregistered_size)
{
    BOOST_CHECK(cuckatoo::bench::Lookup(3) == nullptr);
}

BOOST_AUTO_TEST_CASE(register_then_lookup_round_trips)
{
    cuckatoo::bench::SolverVTable vt{};
    vt.create = [](unsigned) -> void* { return nullptr; };
    vt.destroy = [](void*) {};
    vt.solve_one = [](void*, const unsigned char*, size_t, uint32_t,
                      cuckatoo::Cycle&) { return false; };
    vt.verify_one = [](const cuckatoo::Cycle&, const unsigned char*, size_t) {
        return false;
    };

    BOOST_CHECK(cuckatoo::bench::Register(200, vt));
    const auto* got = cuckatoo::bench::Lookup(200);
    BOOST_REQUIRE(got != nullptr);
    BOOST_CHECK(got->create != nullptr);
    BOOST_CHECK(got->verify_one != nullptr);
}

// A second Register for a size already taken must fail rather than silently
// replace it: two solvers claiming one graph size means a generated translation
// unit is wrong, and a measurement run must not paper over that.
BOOST_AUTO_TEST_CASE(register_rejects_a_duplicate_size)
{
    cuckatoo::bench::SolverVTable vt{};
    vt.create = [](unsigned) -> void* { return nullptr; };
    vt.destroy = [](void*) {};
    vt.solve_one = [](void*, const unsigned char*, size_t, uint32_t,
                      cuckatoo::Cycle&) { return false; };
    vt.verify_one = [](const cuckatoo::Cycle&, const unsigned char*, size_t) {
        return false;
    };

    BOOST_CHECK(cuckatoo::bench::Register(201, vt));
    BOOST_CHECK(!cuckatoo::bench::Register(201, vt));
}

BOOST_AUTO_TEST_CASE(sizes_are_sorted_ascending)
{
    const auto sizes = cuckatoo::bench::Sizes();
    BOOST_CHECK(std::is_sorted(sizes.begin(), sizes.end()));
}

BOOST_AUTO_TEST_SUITE_END()
