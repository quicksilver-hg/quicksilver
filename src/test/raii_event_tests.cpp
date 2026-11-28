// Copyright (c) 2016-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <event2/event.h>

#include <cstdlib>
#include <map>

#include <support/events.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(raii_event_tests, BasicTestingSetup)

#ifdef EVENT_SET_MEM_FUNCTIONS_IMPLEMENTED

static std::map<void*, short> tags;

static void* tag_malloc(size_t sz) {
    void* mem = malloc(sz);
    if (!mem) return mem;
    tags[mem]++;
    return mem;
}

static void tag_free(void* mem) {
    tags[mem]--;
    free(mem);
}

struct ScopedTaggedAllocators {
    ScopedTaggedAllocators() { event_set_mem_functions(tag_malloc, realloc, tag_free); }
    ~ScopedTaggedAllocators() { event_set_mem_functions(malloc, realloc, free); }
};

BOOST_AUTO_TEST_CASE(raii_event_creation)
{
    ScopedTaggedAllocators allocators;

    void* base_ptr = nullptr;
    {
        auto base = obtain_event_base();
        base_ptr = (void*)base.get();
        BOOST_CHECK(tags[base_ptr] == 1);
    }
    BOOST_CHECK(tags[base_ptr] == 0);
}

#endif  // EVENT_SET_MEM_FUNCTIONS_IMPLEMENTED

BOOST_AUTO_TEST_SUITE_END()
