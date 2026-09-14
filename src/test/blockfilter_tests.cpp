// Copyright (c) 2018-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/data/blockfilters.json.h>
#include <test/util/setup_common.h>

#include <blockfilter.h>
#include <chain.h>
#include <hash.h>
#include <node/blockstorage.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <serialize.h>
#include <span.h>
#include <streams.h>
#include <sync.h>
#include <tinyformat.h>
#include <undo.h>
#include <univalue.h>
#include <util/check.h>
#include <util/strencodings.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <ios>
#include <vector>

BOOST_AUTO_TEST_SUITE(blockfilter_tests)

namespace {
//! Reads a block in Bitcoin's serialization -- the format the BIP 158 vectors are
//! recorded in -- and reports the output scripts it creates, grouped per transaction.
//!
//! This deliberately does not go through CBlock's deserializer, and hand-rolls compact
//! size decoding instead of calling ReadCompactSize. Quicksilver's header, block and
//! transaction formats have every one of them diverged from Bitcoin's serialization,
//! while these vectors are fixed external data recorded against the format BIP 158 was
//! specified over. An oracle that decoded them with our own serialization stack would
//! silently follow that stack wherever it moved -- which is exactly how the previous copy
//! of blockfilters.json came to hold re-derived numbers that agreed with nothing but
//! ourselves.
class BitcoinFormatBlockReader
{
    const std::vector<unsigned char>& m_data;
    size_t m_pos{0};

    void Require(uint64_t count) const
    {
        if (m_data.size() - m_pos < count) {
            throw std::ios_base::failure("BIP 158 vector block is truncated");
        }
    }

public:
    explicit BitcoinFormatBlockReader(const std::vector<unsigned char>& data) : m_data(data) {}

    bool AtEnd() const { return m_pos == m_data.size(); }

    void Skip(uint64_t count)
    {
        Require(count);
        m_pos += count;
    }

    uint64_t ReadLE(size_t width)
    {
        Require(width);
        uint64_t value{0};
        for (size_t i = 0; i < width; ++i) {
            value |= static_cast<uint64_t>(m_data[m_pos + i]) << (8 * i);
        }
        m_pos += width;
        return value;
    }

    uint64_t ReadCompactSize()
    {
        const uint64_t first{ReadLE(1)};
        if (first < 253) return first;
        if (first == 253) return ReadLE(2);
        if (first == 254) return ReadLE(4);
        return ReadLE(8);
    }

    CScript ReadLengthPrefixedScript()
    {
        const uint64_t length{ReadCompactSize()};
        Require(length);
        const auto begin{m_data.begin() + static_cast<ptrdiff_t>(m_pos)};
        m_pos += length;
        return CScript(begin, begin + static_cast<ptrdiff_t>(length));
    }
};

struct ParsedBitcoinBlock {
    std::vector<std::vector<CScript>> tx_output_scripts;
    bool has_witness_data{false};
};

ParsedBitcoinBlock ParseBitcoinFormatBlock(const std::vector<unsigned char>& raw)
{
    BitcoinFormatBlockReader reader{raw};
    reader.Skip(80); // fixed-width 80-byte block header

    ParsedBitcoinBlock parsed;
    const uint64_t tx_count{reader.ReadCompactSize()};
    for (uint64_t i = 0; i < tx_count; ++i) {
        reader.Skip(4); // version

        // BIP 144: an input count of zero is the witness marker, not an input count.
        uint64_t input_count{reader.ReadCompactSize()};
        const bool witness{input_count == 0};
        if (witness) {
            if (reader.ReadLE(1) != 1) throw std::ios_base::failure("bad witness flag");
            input_count = reader.ReadCompactSize();
            parsed.has_witness_data = true;
        }
        for (uint64_t j = 0; j < input_count; ++j) {
            reader.Skip(32 + 4);            // prevout
            reader.ReadLengthPrefixedScript(); // scriptSig
            reader.Skip(4);                 // sequence
        }

        std::vector<CScript> outputs;
        const uint64_t output_count{reader.ReadCompactSize()};
        for (uint64_t j = 0; j < output_count; ++j) {
            reader.Skip(8); // value
            outputs.push_back(reader.ReadLengthPrefixedScript());
        }
        parsed.tx_output_scripts.push_back(std::move(outputs));

        if (witness) {
            for (uint64_t j = 0; j < input_count; ++j) {
                const uint64_t stack_size{reader.ReadCompactSize()};
                for (uint64_t k = 0; k < stack_size; ++k) reader.Skip(reader.ReadCompactSize());
            }
        }
        reader.Skip(4); // locktime
    }

    // A vector whose block hex we only partly consumed would quietly contribute a short
    // element set and still encode to something -- just not to the expected filter. Fail
    // where the cause is legible instead.
    if (!reader.AtEnd()) throw std::ios_base::failure("trailing data after BIP 158 vector block");
    return parsed;
}
} // namespace

BOOST_AUTO_TEST_CASE(gcsfilter_test)
{
    GCSFilter::ElementSet included_elements, excluded_elements;
    for (int i = 0; i < 100; ++i) {
        GCSFilter::Element element1(32);
        element1[0] = i;
        included_elements.insert(std::move(element1));

        GCSFilter::Element element2(32);
        element2[1] = i;
        excluded_elements.insert(std::move(element2));
    }

    GCSFilter filter({0, 0, 10, 1 << 10}, included_elements);
    for (const auto& element : included_elements) {
        BOOST_CHECK(filter.Match(element));

        auto insertion = excluded_elements.insert(element);
        BOOST_CHECK(filter.MatchAny(excluded_elements));
        excluded_elements.erase(insertion.first);
    }
}

BOOST_AUTO_TEST_CASE(gcsfilter_default_constructor)
{
    GCSFilter filter;
    BOOST_CHECK_EQUAL(filter.GetN(), 0U);
    BOOST_CHECK_EQUAL(filter.GetEncoded().size(), 1U);

    const GCSFilter::Params& params = filter.GetParams();
    BOOST_CHECK_EQUAL(params.m_siphash_k0, 0U);
    BOOST_CHECK_EQUAL(params.m_siphash_k1, 0U);
    BOOST_CHECK_EQUAL(params.m_P, 0);
    BOOST_CHECK_EQUAL(params.m_M, 1U);
}

BOOST_AUTO_TEST_CASE(gcsfilter_encoding_independent_of_insertion_order)
{
    // GCSFilter::ElementSet is a std::unordered_set, so its iteration order is not part of
    // the filter's definition. What makes that safe is the std::sort in
    // GCSFilter::BuildHashedSet -- one line, and until this test nothing pinned it. It is
    // worth pinning because filters sit outside consensus: two nodes that encoded the same
    // block differently would stay in perfect agreement about the chain while serving
    // permanently contradictory filters, and no rule, reorg or peer exchange would surface
    // it.
    std::vector<GCSFilter::Element> elements;
    for (int i = 0; i < 100; ++i) {
        GCSFilter::Element element(32);
        element[0] = static_cast<unsigned char>(i);
        element[1] = static_cast<unsigned char>(i * 7);
        elements.push_back(std::move(element));
    }

    GCSFilter::ElementSet forward_insert, reverse_insert;
    for (auto it = elements.begin(); it != elements.end(); ++it) forward_insert.insert(*it);
    for (auto it = elements.rbegin(); it != elements.rend(); ++it) reverse_insert.insert(*it);
    BOOST_REQUIRE_EQUAL(forward_insert.size(), elements.size());
    BOOST_REQUIRE_EQUAL(reverse_insert.size(), elements.size());

    // The assertion below is only meaningful if the two containers genuinely iterate in
    // different orders. If they ever iterate identically, byte-equality would hold with the
    // sort deleted and this test would pin nothing, so fail loudly here instead of passing
    // vacuously. The fix is different elements -- never deleting this check.
    const std::vector<GCSFilter::Element> forward_order(forward_insert.begin(), forward_insert.end());
    const std::vector<GCSFilter::Element> reverse_order(reverse_insert.begin(), reverse_insert.end());
    BOOST_REQUIRE(forward_order != reverse_order);

    const GCSFilter::Params params{0, 0, 10, 1 << 10};
    BOOST_CHECK(GCSFilter(params, forward_insert).GetEncoded() ==
                GCSFilter(params, reverse_insert).GetEncoded());

    // Mutation note, so nobody re-derives this the hard way: deleting the std::sort does
    // not make this check fail, it makes the encoder HANG. GCSFilter::Encode emits
    // Golomb-Rice deltas between successive hashed values, so out-of-order values underflow
    // to near-2^64 and the unary part becomes an effectively unbounded run of bits. The
    // sort is therefore not merely a canonicalisation -- it is what keeps the encoder
    // terminating. Run that mutation with a timeout, and read a hang as this test failing.
    // (Unreachable from untrusted input: BuildHashedSet is the only path in and it always
    // sorts.)
}

BOOST_AUTO_TEST_CASE(blockfilter_all_elements_skipped)
{
    // Every output script here is one BasicFilterElements refuses to add, and the undo
    // data's only prevout script is empty. The element set is therefore empty and the
    // encoded filter is the empty GCS -- a single zero byte for N=0. blockfilter_basic_test
    // covers each skip branch individually; this covers all of them at once, which is the
    // case that produces a degenerate filter rather than a smaller one.
    CScript op_return_script;
    op_return_script << OP_RETURN << std::vector<unsigned char>(4, 40);

    CMutableTransaction tx;
    tx.vout.emplace_back(100, op_return_script);
    tx.vout.emplace_back(200, CScript()); // empty output script

    CBlock block;
    block.vtx.push_back(MakeTransactionRef(tx));

    CBlockUndo block_undo;
    block_undo.vtxundo.emplace_back();
    block_undo.vtxundo.back().vprevout.emplace_back(CTxOut(300, CScript()), 1000, false);

    BlockFilter block_filter(BlockFilterType::BASIC, block, block_undo);
    BOOST_CHECK_EQUAL(block_filter.GetFilter().GetN(), 0U);
    const std::vector<unsigned char> empty_gcs{0x00};
    BOOST_CHECK(block_filter.GetEncodedFilter() == empty_gcs);
}

BOOST_FIXTURE_TEST_CASE(blockfilter_header_chain_at_testchain100_tip, TestChain100Setup)
{
    // One constant covering extraction, encoding and header chaining together, over a real
    // chain rather than hand-built blocks. Any change to how elements are selected, how the
    // filter is encoded, or how headers chain moves this line and nothing else -- the same
    // trick TestChain100Setup already plays with its committed tip hash.
    LOCK(cs_main);
    const CChain& chain = m_node.chainman->ActiveChain();
    BOOST_REQUIRE_EQUAL(chain.Height(), 100);

    uint256 filter_header; // the genesis filter chains onto a null header
    for (int height = 0; height <= chain.Height(); ++height) {
        const CBlockIndex* pindex = chain[height];
        BOOST_REQUIRE(pindex != nullptr);

        CBlock block;
        BOOST_REQUIRE(m_node.chainman->m_blockman.ReadBlock(block, *pindex));

        // Genesis has no undo data; every other block needs it, because a spent output's
        // script is only recoverable from there.
        CBlockUndo block_undo;
        if (height > 0) {
            BOOST_REQUIRE(m_node.chainman->m_blockman.ReadBlockUndo(block_undo, *pindex));
        }

        filter_header = BlockFilter(BlockFilterType::BASIC, block, block_undo).ComputeHeader(filter_header);
    }

    // Derived from a run, not predicted.
    //
    // WARNING: if this goes red, do not regenerate the constant to make it pass. It moves
    // for exactly two reasons. Either the block header format changed -- which moves every
    // block hash, and the filter is keyed on the block hash, so TestChain100Setup's own tip
    // hash assertion moves in the same commit -- or filter extraction, encoding or header
    // chaining changed, which is the defect this test exists to catch. Only the first is a
    // reason to re-derive, and it wants a note here saying which remint did it.
    BOOST_CHECK_EQUAL(filter_header.ToString(),
                      "df4bc0420251ec7b7491b49ecb6e4bf2659198b859b17844d7cacd096a4078ba");
}

BOOST_AUTO_TEST_CASE(blockfilter_basic_test)
{
    CScript included_scripts[5], excluded_scripts[4];

    // First two are outputs on a single transaction.
    included_scripts[0] << std::vector<unsigned char>(0, 65) << OP_CHECKSIG;
    included_scripts[1] << OP_DUP << OP_HASH160 << std::vector<unsigned char>(1, 20) << OP_EQUALVERIFY << OP_CHECKSIG;

    // Third is an output on in a second transaction.
    included_scripts[2] << OP_1 << std::vector<unsigned char>(2, 33) << OP_1 << OP_CHECKMULTISIG;

    // Last two are spent by a single transaction.
    included_scripts[3] << OP_0 << std::vector<unsigned char>(3, 32);
    included_scripts[4] << OP_4 << OP_ADD << OP_8 << OP_EQUAL;

    // OP_RETURN output is an output on the second transaction.
    excluded_scripts[0] << OP_RETURN << std::vector<unsigned char>(4, 40);

    // This script is not related to the block at all.
    excluded_scripts[1] << std::vector<unsigned char>(5, 33) << OP_CHECKSIG;

    // OP_RETURN is non-standard since it's not followed by a data push, but is still excluded from
    // filter.
    excluded_scripts[2] << OP_RETURN << OP_4 << OP_ADD << OP_8 << OP_EQUAL;

    CMutableTransaction tx_1;
    tx_1.vout.emplace_back(100, included_scripts[0]);
    tx_1.vout.emplace_back(200, included_scripts[1]);
    tx_1.vout.emplace_back(0, excluded_scripts[0]);

    CMutableTransaction tx_2;
    tx_2.vout.emplace_back(300, included_scripts[2]);
    tx_2.vout.emplace_back(0, excluded_scripts[2]);
    tx_2.vout.emplace_back(400, excluded_scripts[3]); // Script is empty

    CBlock block;
    block.vtx.push_back(MakeTransactionRef(tx_1));
    block.vtx.push_back(MakeTransactionRef(tx_2));

    CBlockUndo block_undo;
    block_undo.vtxundo.emplace_back();
    block_undo.vtxundo.back().vprevout.emplace_back(CTxOut(500, included_scripts[3]), 1000, true);
    block_undo.vtxundo.back().vprevout.emplace_back(CTxOut(600, included_scripts[4]), 10000, false);
    block_undo.vtxundo.back().vprevout.emplace_back(CTxOut(700, excluded_scripts[3]), 100000, false);

    BlockFilter block_filter(BlockFilterType::BASIC, block, block_undo);
    const GCSFilter& filter = block_filter.GetFilter();

    for (const CScript& script : included_scripts) {
        BOOST_CHECK(filter.Match(GCSFilter::Element(script.begin(), script.end())));
    }
    for (const CScript& script : excluded_scripts) {
        BOOST_CHECK(!filter.Match(GCSFilter::Element(script.begin(), script.end())));
    }

    // Test serialization/unserialization.
    BlockFilter block_filter2;

    DataStream stream{};
    stream << block_filter;
    stream >> block_filter2;

    BOOST_CHECK_EQUAL(block_filter.GetFilterType(), block_filter2.GetFilterType());
    BOOST_CHECK_EQUAL(block_filter.GetBlockHash(), block_filter2.GetBlockHash());
    BOOST_CHECK(block_filter.GetEncodedFilter() == block_filter2.GetEncodedFilter());

    BlockFilter default_ctor_block_filter_1;
    BlockFilter default_ctor_block_filter_2;
    BOOST_CHECK_EQUAL(default_ctor_block_filter_1.GetFilterType(), default_ctor_block_filter_2.GetFilterType());
    BOOST_CHECK_EQUAL(default_ctor_block_filter_1.GetBlockHash(), default_ctor_block_filter_2.GetBlockHash());
    BOOST_CHECK(default_ctor_block_filter_1.GetEncodedFilter() == default_ctor_block_filter_2.GetEncodedFilter());
}

BOOST_AUTO_TEST_CASE(blockfilters_bip158_conformance_test)
{
    // The BIP 158 test vectors, verbatim. These are the only external oracle this
    // implementation has: everything else in this file, and every filter our nodes agree
    // with each other about, is derived from this same code.
    //
    // WARNING: if this goes red, DO NOT regenerate the vectors. They are not ours to
    // re-derive: they are BIP 158's published output over ten fixed Bitcoin blocks, and a
    // filter that disagrees with them is wrong however consistently our own nodes
    // reproduce it. The previous copy of blockfilters.json was re-minted twice to follow
    // our serialization changes, which left a test that could only ever confirm that we
    // still agree with ourselves. Nothing below reads block.GetHash(), decodes a block
    // with CBlock, or reads P and M from the code under test, so no change to
    // Quicksilver's own formats can legitimately move these numbers.
    //
    // Provenance: byte-identical to the pristine 29.1 baseline vendored at 41b6271a, and
    // to https://github.com/bitcoin/bitcoin/blob/master/src/test/data/blockfilters.json as
    // of 2026-08-26. The digest below is what makes that checkable in-tree rather than a
    // claim in a comment: it fails on any edit to the file, re-mint included. Update it
    // only alongside vectors re-fetched from upstream.
    BOOST_CHECK_EQUAL(Hash(json_tests::blockfilters).ToString(),
                      "36c2521eb8c57bdca6d1321b24000189c13dedd4220488f704eb7098fbb3431f");

    UniValue json;
    BOOST_REQUIRE_MESSAGE(json.read(json_tests::blockfilters) && json.isArray(), "Parse error.");
    const UniValue& tests{json.get_array()};

    size_t vectors{0};
    size_t empty_filters{0};
    size_t op_return_outputs{0};
    size_t empty_output_scripts{0};
    size_t vectors_with_spent_scripts{0};
    size_t witness_blocks{0};

    for (unsigned int i = 0; i < tests.size(); ++i) {
        const UniValue& test{tests[i]};
        if (test.size() == 1) continue; // the leading row names the columns
        BOOST_REQUIRE_MESSAGE(test.size() >= 7, "Bad test: " << test.write());
        ++vectors;

        unsigned int pos{0};
        const int block_height{test[pos++].getInt<int>()};
        const uint256 block_hash{*Assert(uint256::FromHex(test[pos++].get_str()))};
        const std::vector<unsigned char> raw_block{ParseHex(test[pos++].get_str())};
        const UniValue& prev_scripts{test[pos++].get_array()};
        const uint256 prev_header{*Assert(uint256::FromHex(test[pos++].get_str()))};
        const std::vector<unsigned char> expected_filter{ParseHex(test[pos++].get_str())};
        const uint256 expected_header{*Assert(uint256::FromHex(test[pos++].get_str()))};
        const std::string at{strprintf("BIP 158 vector at height %d", block_height)};

        // Re-house the vector's output scripts in a CBlock purely so that the real
        // element-selection code can run over them. Values are irrelevant to a filter and
        // are not recorded per-output by the vectors, so they are left at zero.
        const ParsedBitcoinBlock parsed{ParseBitcoinFormatBlock(raw_block)};
        CBlock block;
        for (const std::vector<CScript>& outputs : parsed.tx_output_scripts) {
            CMutableTransaction tx;
            for (const CScript& script : outputs) {
                tx.vout.emplace_back(0, script);
                if (script.empty()) {
                    ++empty_output_scripts;
                } else if (script[0] == OP_RETURN) {
                    ++op_return_outputs;
                }
            }
            block.vtx.push_back(MakeTransactionRef(std::move(tx)));
        }
        witness_blocks += parsed.has_witness_data;

        CBlockUndo block_undo;
        block_undo.vtxundo.emplace_back();
        for (unsigned int j = 0; j < prev_scripts.size(); ++j) {
            const std::vector<unsigned char> raw{ParseHex(prev_scripts[j].get_str())};
            block_undo.vtxundo.back().vprevout.emplace_back(
                CTxOut(0, CScript(raw.begin(), raw.end())), 0, false);
        }
        vectors_with_spent_scripts += prev_scripts.size() > 0;

        // Reconstructing from the vector's own filter bytes does three things at once: it
        // runs BuildParams, so the siphash key derivation and the P/M constants are under
        // test rather than restated here; it makes the GCS decoder check the encoding
        // against data it did not produce; and it gives ComputeHeader an input that is
        // right even if our encoder is not, so a header failure means a header defect.
        const BlockFilter from_vector{BlockFilterType::BASIC, block_hash, expected_filter,
                                      /*skip_decode_check=*/false};
        const GCSFilter::Params& params{from_vector.GetFilter().GetParams()};
        BOOST_CHECK_EQUAL(unsigned{params.m_P}, 19U);
        BOOST_CHECK_EQUAL(params.m_M, 784931U);
        BOOST_CHECK_EQUAL(params.m_siphash_k0, block_hash.GetUint64(0));
        BOOST_CHECK_EQUAL(params.m_siphash_k1, block_hash.GetUint64(1));

        const GCSFilter::ElementSet elements{BasicFilterElements(block, block_undo)};
        const GCSFilter filter{params, elements};
        empty_filters += elements.empty();

        BOOST_CHECK_MESSAGE(filter.GetEncoded() == expected_filter,
                            at << ": encoded " << HexStr(filter.GetEncoded())
                               << ", BIP 158 says " << HexStr(expected_filter));
        BOOST_CHECK_MESSAGE(filter.GetN() == elements.size(),
                            at << ": filter claims " << filter.GetN() << " of "
                               << elements.size() << " elements");
        BOOST_CHECK_MESSAGE(from_vector.ComputeHeader(prev_header) == expected_header,
                            at << ": header " << from_vector.ComputeHeader(prev_header).ToString()
                               << ", BIP 158 says " << expected_header.ToString());
    }

    // The loop above passes vacuously over an empty or truncated file, and passes almost
    // vacuously over a file trimmed to the easy rows -- ten near-identical single-output
    // blocks would exercise one branch of BasicFilterElements and one shape of filter. Pin
    // what makes this set worth running. The digest check above already fails on any edit;
    // these say what would have been lost.
    BOOST_CHECK_EQUAL(vectors, 10U);
    BOOST_CHECK(empty_filters >= 1);            // N == 0, the degenerate encoding
    BOOST_CHECK(op_return_outputs >= 1);        // the OP_RETURN skip branch
    BOOST_CHECK(empty_output_scripts >= 1);     // the empty-script skip branch
    BOOST_CHECK(vectors_with_spent_scripts >= 1); // elements recovered from undo data
    BOOST_CHECK(witness_blocks >= 1);           // a block carrying witness data
}

BOOST_AUTO_TEST_CASE(blockfilter_type_names)
{
    BOOST_CHECK_EQUAL(BlockFilterTypeName(BlockFilterType::BASIC), "basic");
    BOOST_CHECK_EQUAL(BlockFilterTypeName(static_cast<BlockFilterType>(255)), "");

    BlockFilterType filter_type;
    BOOST_CHECK(BlockFilterTypeByName("basic", filter_type));
    BOOST_CHECK_EQUAL(filter_type, BlockFilterType::BASIC);

    BOOST_CHECK(!BlockFilterTypeByName("unknown", filter_type));
}

BOOST_AUTO_TEST_SUITE_END()
