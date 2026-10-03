// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// F-265 capture: the vendored lean solver's graph logs a links[] overflow and a
// compressor node overflow once per graph, with the data needed to replay the
// graph, and otherwise behaves exactly as before (owner ruling 2026-10-01).

#include <crypto/cuckatoo/cuckatoo.h>
#include <crypto/cuckatoo/vendor_prelude_solve.h>

#define EDGEBITS 19
#define PROOFSIZE 42
#define NSIPHASH 1
#define ATOMIC
#define SQUASH_OUTPUT 1
#define CUCKATOO_NO_MAIN
namespace cuckatoo_f265_capture_e19 {
#include <crypto/cuckatoo/vendor/lean.cpp>
}
#undef EDGEBITS
#undef PROOFSIZE
#undef NSIPHASH
#undef ATOMIC
#undef SQUASH_OUTPUT
#undef CUCKATOO_NO_MAIN
#undef MAXEDGES // lean.hpp's macro would otherwise rewrite graph::MAXEDGES below

#include <boost/test/unit_test.hpp>

#include <cstdio>
#include <string>
#include <vector>

namespace {

namespace E = cuckatoo_f265_capture_e19;
using Graph = E::graph<E::word_t>;

//! Everything written to `f`, split into lines.
std::vector<std::string> Lines(FILE* f)
{
    std::vector<std::string> lines;
    std::string cur;
    std::rewind(f);
    for (int c; (c = std::fgetc(f)) != EOF;) {
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur.push_back((char)c);
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

bool Has(const std::string& line, const std::string& part) { return line.find(part) != std::string::npos; }

const char HDR[] = {'\x0a', '\x0b', '\xfe'};

void SetReplay(Graph& cg, FILE* out)
{
    cg.f265_hdr = HDR;
    cg.f265_len = sizeof(HDR);
    cg.f265_nonce = 7;
    cg.f265_nthreads = 4;
    cg.f265_out = out;
}

} // namespace

BOOST_AUTO_TEST_SUITE(cuckatoo_f265_capture_tests)

BOOST_AUTO_TEST_CASE(link_overflow_is_logged_once_per_graph_and_the_edge_is_still_added)
{
    // Allocate room for 8 edges, then lower the bound to 2. The overflowing writes stay
    // inside the real allocation, so the test is memory-safe and can watch add_edge carry on.
    Graph cg(/*maxedges=*/8, /*maxnodes=*/64, /*maxsols=*/4, /*compressbits=*/0);
    cg.MAXEDGES = 2;
    cg.reset();
    FILE* out = std::tmpfile();
    BOOST_REQUIRE(out);
    SetReplay(cg, out);

    BOOST_CHECK(cg.add_edge(0, 1));
    BOOST_CHECK(cg.add_edge(2, 3));
    BOOST_CHECK(Lines(out).empty()); // 4 halfedges is exactly full, not over

    BOOST_CHECK(cg.add_edge(4, 5));
    BOOST_CHECK(cg.add_edge(6, 7));
    BOOST_CHECK_EQUAL(cg.nlinks, 8U); // the solve is unchanged: both edges were added

    std::vector<std::string> lines = Lines(out);
    BOOST_REQUIRE_EQUAL(lines.size(), 1U);
    const std::string& l = lines[0];
    BOOST_CHECK_MESSAGE(l.rfind("F265-CAPTURE LINK OVERFLOW a=4 b=5 ", 0) == 0, l);
    BOOST_CHECK_MESSAGE(Has(l, " nlinks=4 MAXEDGES=2 MAXNODES=64 EDGEBITS=19 nthreads=4 nonce=7 len=3 hdr=0a0bfe"), l);
    BOOST_CHECK_MESSAGE(l.size() >= 6 && l.compare(l.size() - 6, 6, "0a0bfe") == 0, l);

    // A new graph logs again.
    cg.reset();
    cg.add_edge(0, 1);
    cg.add_edge(2, 3);
    cg.add_edge(4, 5);
    BOOST_CHECK_EQUAL(Lines(out).size(), 2U);
    std::fclose(out);
}

BOOST_AUTO_TEST_CASE(compressor_node_overflow_is_logged_once_per_graph)
{
    // compressbits 17 at EDGEBITS 19 leaves a 4-entry table: room for 2 node pairs.
    Graph cg(/*maxedges=*/8, /*maxnodes=*/8, /*maxsols=*/4, /*compressbits=*/17);
    cg.reset();
    FILE* out = std::tmpfile();
    BOOST_REQUIRE(out);
    SetReplay(cg, out);

    cg.add_compress_edge(2, 2);
    cg.add_compress_edge(4, 4);
    BOOST_CHECK(Lines(out).empty());
    BOOST_CHECK_EQUAL(cg.compressu->overflows, 0U);

    cg.add_compress_edge(6, 6); // a third pair on each side: both tables overflow
    cg.add_compress_edge(8, 8);
    BOOST_CHECK_EQUAL(cg.compressu->overflows, 2U);
    BOOST_CHECK_EQUAL(cg.nlinks, 8U);

    std::vector<std::string> lines = Lines(out);
    BOOST_REQUIRE_EQUAL(lines.size(), 1U);
    BOOST_CHECK_MESSAGE(lines[0].rfind("F265-CAPTURE NODE OVERFLOW a=2 b=2 ", 0) == 0, lines[0]);
    BOOST_CHECK_MESSAGE(Has(lines[0], " nonce=7 len=3 hdr=0a0bfe"), lines[0]);

    cg.reset();
    BOOST_CHECK_EQUAL(cg.compressu->overflows, 0U);
    std::fclose(out);
}

BOOST_AUTO_TEST_SUITE_END()
