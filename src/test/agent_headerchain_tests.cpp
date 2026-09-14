// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <agent/agentclient.h>
#include <agent/agentpeer.h>
#include <agent/agentpeerset.h>
#include <agent/headerchain.h>
#include <agent/headerdriver.h>
#include <agent/headermessages.h>
#include <agent/headerpeer.h>
#include <agent/headerstore.h>
#include <agent/headersync.h>
#include <agent/messageio.h>
#include <agent/peertransport.h>
#include <agent/txmessages.h>
#include <agent/txpeer.h>
#include <agent/allotmentpolicy.h>
#include <agent/allotmentspend.h>
#include <agent/interrupt.h>
#include <agent/allotmentstore.h>

#include <addresstype.h>
#include <chainparams.h>
#include <consensus/amount.h>
#include <key.h>
#include <key_io.h>
#include <netmessagemaker.h>
#include <policy/policy.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <protocol.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <streams.h>
#include <test/util/mining.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <uint256.h>
#include <util/fs.h>
#include <util/readwritefile.h>
#include <util/result.h>
#include <util/string.h>
#include <util/time.h>
#include <versionbits.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <csignal>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace {

constexpr uint32_t TEST_HEADER_STORE_MAGIC{0x47415351};
constexpr uint32_t TEST_HEADER_STORE_VERSION{1};

struct AgentHeaderChainTestingSetup : public BasicTestingSetup {
    AgentHeaderChainTestingSetup()
        : BasicTestingSetup{ChainType::SANDBOX}
    {
        SetMockTime(Params().GenesisBlock().nTime + 100000);
    }

    CBlockHeader NextHeaderFrom(const CBlockIndex& prev, int64_t time = 0, uint256 merkle_root = uint256{},
                                uint32_t congestion = static_cast<uint32_t>(CONGESTION_ONE))
    {
        CBlockHeader header;
        header.nVersion = VERSIONBITS_LAST_OLD_BLOCK_VERSION;
        header.hashPrevBlock = prev.GetBlockHash();
        header.hashMerkleRoot = merkle_root;
        header.nTime = time == 0 ? prev.GetMedianTimePast() + 1 : time;
        header.nBits = GetNextWorkRequired(&prev, &header, Params().GetConsensus());
        // A real header off the wire carries the multiplier; the floor is what a chain
        // of under-target blocks settles at. Set before SolveBlockPoW: nCongestion is
        // inside the pre-pow, so the cycle is bound to it.
        header.nCongestion = congestion;
        SolveBlockPoW(header, Params().GetConsensus());
        return header;
    }

    CBlockHeader NextHeader(const agent::HeaderChain& chain, int64_t time = 0, uint256 merkle_root = uint256{},
                            uint32_t congestion = static_cast<uint32_t>(CONGESTION_ONE))
    {
        return NextHeaderFrom(chain.Tip(), time, merkle_root, congestion);
    }

    CBlockHeader BadPowHeader(const agent::HeaderChain& chain)
    {
        CBlockHeader header{NextHeader(chain)};
        while (CheckProofOfWork(header, Params().GetConsensus())) {
            ++header.nCycle[0];
        }
        return header;
    }

    fs::path HeaderStorePath(const char* name) const { return m_path_root / name; }

    std::unique_ptr<agent::HeaderChain> ChainWithHeaders(int count)
    {
        auto chain{std::make_unique<agent::HeaderChain>(Params().GetConsensus(), Params().GenesisBlock())};
        for (int i{0}; i < count; ++i) {
            BOOST_REQUIRE(chain->AcceptHeader(NextHeader(*chain, 0, ArithToUint256(i + 1))).accepted());
        }
        return chain;
    }

    CTransactionRef TestTransaction(uint32_t anchor_height = 10, bool witness = true)
    {
        CMutableTransaction transaction;
        transaction.vin.emplace_back(COutPoint{Txid::FromUint256(ArithToUint256(1)), 0});
        transaction.vout.emplace_back(CAmount{1}, CScript{});
        transaction.nAnchorHeight = anchor_height;
        transaction.nPowNonce = 99;
        transaction.nCycle[0] = 7;
        transaction.nCycle[41] = 42;
        if (witness) {
            transaction.vin.front().scriptWitness.stack.push_back({0x01, 0x02, 0x03});
        }
        return MakeTransactionRef(transaction);
    }

    void WriteRawHeaderStore(const fs::path& path,
                             uint32_t magic,
                             uint32_t version,
                             const uint256& genesis_hash,
                             const std::vector<CBlockHeader>& headers,
                             std::optional<uint64_t> count = std::nullopt)
    {
        AutoFile file{fsbridge::fopen(path, "wb")};
        BOOST_REQUIRE(!file.IsNull());

        file << magic;
        file << version;
        file << genesis_hash;
        file << count.value_or(headers.size());
        for (const CBlockHeader& header : headers) {
            file << header;
        }

        BOOST_REQUIRE(file.Commit());
        file.fclose();
    }
};

std::string AgentAllotmentBundleJson(const CKey& funding_key,
                                  CAmount funding_available = COIN,
                                  CAmount daily_limit = COIN / 2,
                                  const std::vector<agent::AllotmentFundingOutputArtifact>& funding_outputs = {})
{
    const std::string funding_address{EncodeDestination(WitnessV0KeyHash(funding_key.GetPubKey()))};
    const std::string policy_request{strprintf(
        R"({"type":"quicksilver.agent_allotment_policy_request","version":1,"chain":"%s","genesis_hash":"%s","id":"agent-1","label":"test-agent","funding_address":"%s","funding_limit_cinnabar":"%s","funding_available_cinnabar":"%s","daily_limit_cinnabar":"%s","risk_accepted_time":"123","request_created_time":"456","policy_status":"pending_integration","backend_created":false})",
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash().ToString(),
        funding_address,
        util::ToString(COIN),
        util::ToString(funding_available),
        util::ToString(daily_limit))};

    std::string funding_outputs_json;
    if (!funding_outputs.empty()) {
        funding_outputs_json = R"(,"funding_outputs":[)";
        for (size_t i{0}; i < funding_outputs.size(); ++i) {
            if (i > 0) funding_outputs_json += ",";
            funding_outputs_json += strprintf(
                R"({"txid":"%s","vout":%s,"amount_cinnabar":"%s"})",
                funding_outputs[i].txid,
                util::ToString(funding_outputs[i].vout),
                util::ToString(funding_outputs[i].amount));
        }
        funding_outputs_json += "]";
    }

    return strprintf(
        R"({"type":"quicksilver.agent_allotment_key_bundle","version":1,"policy_request":%s,"funding_address":"%s","funding_secret_wif":"%s"%s,"policy_enforcement":"pending_integration"})",
        policy_request,
        funding_address,
        EncodeSecret(funding_key),
        funding_outputs_json);
}

std::string AgentAllotmentPaymentReceiptJson(const std::string& funding_address,
                                          const agent::AllotmentFundingOutputArtifact& funding_output,
                                          int64_t received_time = 789,
                                          std::string_view metadata_json = {})
{
    return strprintf(
        R"({"type":"quicksilver.agent_payment_receipt","version":1,"chain":"%s","genesis_hash":"%s","funding_address":"%s","txid":"%s","vout":%s,"amount_cinnabar":"%s","received_time":"%s"%s})",
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash().ToString(),
        funding_address,
        funding_output.txid,
        util::ToString(funding_output.vout),
        util::ToString(funding_output.amount),
        util::ToString(received_time),
        metadata_json);
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(agent_headerchain_tests, AgentHeaderChainTestingSetup)

BOOST_AUTO_TEST_CASE(fixed_seed_peers_follow_selected_chain)
{
    BOOST_CHECK(agent::FixedSeedPeers().empty());

    const auto main_params{CreateChainParams(*m_node.args, ChainType::MAIN)};
    const std::vector<CService> main_seeds{agent::FixedSeedPeers(*main_params)};
    BOOST_REQUIRE_EQUAL(main_seeds.size(), 1U);
    BOOST_CHECK(main_seeds.front().IsTor());
    BOOST_CHECK_EQUAL(main_seeds.front().GetPort(), 9555);

    const auto publictest_params{CreateChainParams(*m_node.args, ChainType::PUBLIC_TEST)};
    const std::vector<CService> publictest_seeds{agent::FixedSeedPeers(*publictest_params)};
    BOOST_REQUIRE_EQUAL(publictest_seeds.size(), 1U);
    BOOST_CHECK(publictest_seeds.front().IsTor());
    BOOST_CHECK_EQUAL(publictest_seeds.front().GetPort(), 19557);
}

BOOST_AUTO_TEST_CASE(header_chain_starts_at_genesis)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};

    BOOST_CHECK_EQUAL(chain.Height(), 0);
    BOOST_CHECK(chain.Tip().GetBlockHash() == Params().GenesisBlock().GetHash());
    BOOST_CHECK(chain.Lookup(Params().GenesisBlock().GetHash()) == &chain.Genesis());
    BOOST_CHECK(chain.ChainWork() > arith_uint256{0});
}

BOOST_AUTO_TEST_CASE(accepts_contiguous_headers_and_builds_locator)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    std::vector<uint256> accepted_hashes;

    for (int i{0}; i < 16; ++i) {
        const CBlockHeader header{NextHeader(chain, 0, ArithToUint256(i + 1))};
        const auto result{chain.AcceptHeader(header)};
        BOOST_CHECK(result.accepted());
        BOOST_CHECK_EQUAL(result.height, i + 1);
        accepted_hashes.push_back(header.GetHash());
    }

    BOOST_CHECK_EQUAL(chain.Height(), 16);
    BOOST_CHECK(chain.Tip().GetBlockHash() == accepted_hashes.back());

    const CBlockLocator locator{chain.GetLocator()};
    BOOST_REQUIRE(!locator.vHave.empty());
    BOOST_CHECK(locator.vHave.front() == chain.Tip().GetBlockHash());
    BOOST_CHECK(locator.vHave.back() == Params().GenesisBlock().GetHash());
}

BOOST_AUTO_TEST_CASE(rejects_disconnected_headers)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    const CBlockHeader first{NextHeader(chain)};
    BOOST_REQUIRE(chain.AcceptHeader(first).accepted());

    CBlockHeader disconnected{NextHeader(chain)};
    disconnected.hashPrevBlock = ArithToUint256(999);
    SolveBlockPoW(disconnected, Params().GetConsensus());
    BOOST_CHECK_EQUAL(chain.AcceptHeader(disconnected).code, agent::HeaderAcceptCode::PREV_NOT_FOUND);
}

BOOST_AUTO_TEST_CASE(accepts_equal_work_fork_without_reorging)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    const CBlockHeader first{NextHeader(chain, 0, ArithToUint256(1))};
    BOOST_REQUIRE(chain.AcceptHeader(first).accepted());
    const uint256 first_hash{chain.Tip().GetBlockHash()};

    // A same-height sibling is valid (parent is known) but does not become tip:
    // first-seen wins on equal work, matching a full node.
    const CBlockHeader fork{NextHeaderFrom(chain.Genesis(), 0, ArithToUint256(2))};
    const auto result{chain.AcceptHeader(fork)};
    BOOST_CHECK(result.accepted());
    BOOST_CHECK_EQUAL(result.height, 1);
    BOOST_CHECK(chain.Tip().GetBlockHash() == first_hash);
    BOOST_CHECK(chain.Lookup(fork.GetHash()) != nullptr);
}

BOOST_AUTO_TEST_CASE(reorgs_to_a_heavier_side_chain)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    const CBlockHeader first{NextHeader(chain, 0, ArithToUint256(1))};
    BOOST_REQUIRE(chain.AcceptHeader(first).accepted());
    BOOST_CHECK_EQUAL(chain.Height(), 1);

    // getheaders after a fork returns headers from the common ancestor. The first
    // of those has a known parent that is not the current tip; the second overtakes.
    const CBlockHeader side_first{NextHeaderFrom(chain.Genesis(), 0, ArithToUint256(2))};
    BOOST_REQUIRE(chain.AcceptHeader(side_first).accepted());
    BOOST_CHECK_EQUAL(chain.Height(), 1);
    BOOST_CHECK(chain.Tip().GetBlockHash() == first.GetHash());

    const CBlockIndex* side_parent{chain.Lookup(side_first.GetHash())};
    BOOST_REQUIRE(side_parent != nullptr);
    const CBlockHeader side_second{NextHeaderFrom(*side_parent, 0, ArithToUint256(3))};
    const auto overtake{chain.AcceptHeader(side_second)};
    BOOST_CHECK(overtake.accepted());
    BOOST_CHECK_EQUAL(overtake.height, 2);
    BOOST_CHECK_EQUAL(chain.Height(), 2);
    BOOST_CHECK(chain.Tip().GetBlockHash() == side_second.GetHash());
    BOOST_CHECK(chain.ChainWork() > chain.Lookup(first.GetHash())->nChainWork);
    BOOST_CHECK(chain.GetLocator().vHave.front() == side_second.GetHash());
    BOOST_CHECK(chain.Lookup(first.GetHash()) != nullptr);
}

BOOST_AUTO_TEST_CASE(persists_and_reloads_a_reorged_header_tree)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    BOOST_REQUIRE(chain.AcceptHeader(NextHeader(chain, 0, ArithToUint256(1))).accepted());
    const CBlockHeader side_first{NextHeaderFrom(chain.Genesis(), 0, ArithToUint256(2))};
    BOOST_REQUIRE(chain.AcceptHeader(side_first).accepted());
    const CBlockIndex* side_parent{chain.Lookup(side_first.GetHash())};
    BOOST_REQUIRE(side_parent != nullptr);
    const CBlockHeader side_second{NextHeaderFrom(*side_parent, 0, ArithToUint256(3))};
    BOOST_REQUIRE(chain.AcceptHeader(side_second).accepted());

    const fs::path path{HeaderStorePath("agent_reorg_headers.dat")};
    BOOST_CHECK_EQUAL(agent::SaveHeaderChain(chain, path), agent::HeaderStoreResult::OK);
    const auto loaded{agent::LoadHeaderChain(Params().GetConsensus(), Params().GenesisBlock(), path)};

    BOOST_REQUIRE(loaded.ok());
    BOOST_REQUIRE(loaded.chain != nullptr);
    BOOST_CHECK_EQUAL(loaded.chain->Height(), 2);
    BOOST_CHECK(loaded.chain->Tip().GetBlockHash() == side_second.GetHash());
    BOOST_CHECK(loaded.chain->Lookup(side_first.GetHash()) != nullptr);
}

BOOST_AUTO_TEST_CASE(headers_sync_accepts_heavier_side_chain_from_common_ancestor)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    BOOST_REQUIRE(chain.AcceptHeader(NextHeader(chain, 0, ArithToUint256(1))).accepted());
    agent::HeaderSyncSession sync{chain, /*max_headers_result=*/8};

    const CBlockHeader side_first{NextHeaderFrom(chain.Genesis(), 0, ArithToUint256(2))};
    agent::HeaderChain served{Params().GetConsensus(), Params().GenesisBlock()};
    BOOST_REQUIRE(served.AcceptHeader(side_first).accepted());
    const CBlockHeader side_second{NextHeader(served, 0, ArithToUint256(3))};
    std::vector<CBlockHeader> headers{side_first, side_second};

    BOOST_REQUIRE(sync.NextHeadersRequest().locator.vHave.front() == chain.Tip().GetBlockHash());
    const auto processed{sync.ProcessHeaders(headers)};

    BOOST_CHECK(processed.ok());
    BOOST_CHECK_EQUAL(processed.code, agent::HeaderSyncResultCode::HEADERS_ACCEPTED);
    BOOST_CHECK_EQUAL(processed.accepted_count, 2U);
    BOOST_CHECK(processed.peer_synced);
    BOOST_CHECK_EQUAL(chain.Height(), 2);
    BOOST_CHECK(chain.Tip().GetBlockHash() == side_second.GetHash());
}

BOOST_AUTO_TEST_CASE(reports_duplicate_headers)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    const CBlockHeader first{NextHeader(chain)};

    BOOST_REQUIRE(chain.AcceptHeader(first).accepted());
    const auto result{chain.AcceptHeader(first)};
    BOOST_CHECK_EQUAL(result.code, agent::HeaderAcceptCode::DUPLICATE);
    BOOST_CHECK_EQUAL(result.height, 1);
}

BOOST_AUTO_TEST_CASE(rejects_bad_proof_of_work)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    const CBlockHeader bad{BadPowHeader(chain)};

    BOOST_CHECK_EQUAL(chain.AcceptHeader(bad).code, agent::HeaderAcceptCode::BAD_POW);
}

BOOST_AUTO_TEST_CASE(rejects_bad_difficulty_bits)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    CBlockHeader bad{NextHeader(chain)};
    --bad.nBits;
    SolveBlockPoW(bad, Params().GetConsensus());

    BOOST_CHECK_EQUAL(chain.AcceptHeader(bad).code, agent::HeaderAcceptCode::BAD_DIFFICULTY);
}

BOOST_AUTO_TEST_CASE(rejects_headers_at_or_before_median_time_past)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    for (int i{0}; i < CBlockIndex::nMedianTimeSpan; ++i) {
        BOOST_REQUIRE(chain.AcceptHeader(NextHeader(chain)).accepted());
    }

    const int64_t median_time{chain.Tip().GetMedianTimePast()};
    CBlockHeader old{NextHeader(chain, median_time)};

    BOOST_CHECK_EQUAL(chain.AcceptHeader(old).code, agent::HeaderAcceptCode::TIME_TOO_OLD);
}

BOOST_AUTO_TEST_CASE(rejects_future_headers)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    CBlockHeader future{NextHeader(chain, TicksSinceEpoch<std::chrono::seconds>(NodeClock::now()) + MAX_FUTURE_BLOCK_TIME + 1)};

    BOOST_CHECK_EQUAL(chain.AcceptHeader(future).code, agent::HeaderAcceptCode::TIME_TOO_NEW);
}

BOOST_AUTO_TEST_CASE(persists_and_loads_headers)
{
    const auto chain{ChainWithHeaders(12)};
    const fs::path path{HeaderStorePath("agent_headers.dat")};

    BOOST_CHECK_EQUAL(agent::SaveHeaderChain(*chain, path), agent::HeaderStoreResult::OK);
    const auto loaded{agent::LoadHeaderChain(Params().GetConsensus(), Params().GenesisBlock(), path)};

    BOOST_REQUIRE(loaded.ok());
    BOOST_REQUIRE(loaded.chain != nullptr);
    BOOST_CHECK_EQUAL(loaded.chain->Height(), chain->Height());
    BOOST_CHECK(loaded.chain->Tip().GetBlockHash() == chain->Tip().GetBlockHash());
    BOOST_CHECK(loaded.chain->Genesis().GetBlockHash() == chain->Genesis().GetBlockHash());
}

BOOST_AUTO_TEST_CASE(reports_missing_header_store)
{
    const auto loaded{agent::LoadHeaderChain(Params().GetConsensus(), Params().GenesisBlock(), HeaderStorePath("missing_headers.dat"))};

    BOOST_CHECK_EQUAL(loaded.status, agent::HeaderStoreResult::FILE_NOT_FOUND);
    BOOST_REQUIRE(loaded.chain != nullptr);
    BOOST_CHECK_EQUAL(loaded.chain->Height(), 0);
}

BOOST_AUTO_TEST_CASE(rejects_header_store_bad_magic)
{
    const auto chain{ChainWithHeaders(1)};
    const fs::path path{HeaderStorePath("bad_magic_headers.dat")};
    WriteRawHeaderStore(path, 0, TEST_HEADER_STORE_VERSION, Params().GenesisBlock().GetHash(), chain->Headers());

    const auto loaded{agent::LoadHeaderChain(Params().GetConsensus(), Params().GenesisBlock(), path)};

    BOOST_CHECK_EQUAL(loaded.status, agent::HeaderStoreResult::BAD_MAGIC);
}

BOOST_AUTO_TEST_CASE(rejects_header_store_bad_genesis)
{
    const auto chain{ChainWithHeaders(1)};
    const fs::path path{HeaderStorePath("bad_genesis_headers.dat")};
    WriteRawHeaderStore(path, TEST_HEADER_STORE_MAGIC, TEST_HEADER_STORE_VERSION, ArithToUint256(123), chain->Headers());

    const auto loaded{agent::LoadHeaderChain(Params().GetConsensus(), Params().GenesisBlock(), path)};

    BOOST_CHECK_EQUAL(loaded.status, agent::HeaderStoreResult::BAD_GENESIS);
}

BOOST_AUTO_TEST_CASE(rejects_empty_header_store)
{
    const fs::path path{HeaderStorePath("empty_headers.dat")};
    WriteRawHeaderStore(path, TEST_HEADER_STORE_MAGIC, TEST_HEADER_STORE_VERSION, Params().GenesisBlock().GetHash(), {}, 0);

    const auto loaded{agent::LoadHeaderChain(Params().GetConsensus(), Params().GenesisBlock(), path)};

    BOOST_CHECK_EQUAL(loaded.status, agent::HeaderStoreResult::EMPTY_STORE);
}

BOOST_AUTO_TEST_CASE(rejects_truncated_header_store)
{
    const auto chain{ChainWithHeaders(2)};
    const fs::path path{HeaderStorePath("truncated_headers.dat")};
    BOOST_REQUIRE_EQUAL(agent::SaveHeaderChain(*chain, path), agent::HeaderStoreResult::OK);
    fs::resize_file(path, 16);

    const auto loaded{agent::LoadHeaderChain(Params().GetConsensus(), Params().GenesisBlock(), path)};

    BOOST_CHECK_EQUAL(loaded.status, agent::HeaderStoreResult::DESERIALIZE_FAILED);
}

BOOST_AUTO_TEST_CASE(revalidates_stored_headers)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    const CBlockHeader bad{BadPowHeader(chain)};
    std::vector<CBlockHeader> headers{Params().GenesisBlock(), bad};
    const fs::path path{HeaderStorePath("invalid_headers.dat")};
    WriteRawHeaderStore(path, TEST_HEADER_STORE_MAGIC, TEST_HEADER_STORE_VERSION, Params().GenesisBlock().GetHash(), headers);

    const auto loaded{agent::LoadHeaderChain(Params().GetConsensus(), Params().GenesisBlock(), path)};

    BOOST_CHECK_EQUAL(loaded.status, agent::HeaderStoreResult::INVALID_HEADER);
    BOOST_CHECK_EQUAL(loaded.invalid_header.code, agent::HeaderAcceptCode::BAD_POW);
}

BOOST_AUTO_TEST_CASE(headers_sync_request_uses_current_locator_and_stop_hash)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    BOOST_REQUIRE(chain.AcceptHeader(NextHeader(chain)).accepted());
    agent::HeaderSyncSession sync{chain};
    const uint256 stop_hash{ArithToUint256(50)};

    const auto request{sync.NextHeadersRequest(stop_hash)};

    BOOST_REQUIRE(sync.HasRequestInFlight());
    BOOST_CHECK(request.stop_hash == stop_hash);
    BOOST_CHECK(sync.LastStopHash() == stop_hash);
    BOOST_REQUIRE(!request.locator.vHave.empty());
    BOOST_CHECK(request.locator.vHave.front() == chain.Tip().GetBlockHash());
}

BOOST_AUTO_TEST_CASE(headers_sync_accepts_requested_headers_and_requests_more_on_full_batch)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    agent::HeaderSyncSession sync{chain, 2};
    agent::HeaderChain served_chain{Params().GetConsensus(), Params().GenesisBlock()};
    const CBlockHeader first{NextHeader(served_chain, 0, ArithToUint256(1))};
    BOOST_REQUIRE(served_chain.AcceptHeader(first).accepted());
    const CBlockHeader second{NextHeader(served_chain, 0, ArithToUint256(2))};
    std::vector<CBlockHeader> headers{first, second};

    BOOST_REQUIRE(sync.NextHeadersRequest().locator.vHave.front() == Params().GenesisBlock().GetHash());
    const auto processed{sync.ProcessHeaders(headers)};

    BOOST_CHECK(processed.ok());
    BOOST_CHECK_EQUAL(processed.code, agent::HeaderSyncResultCode::HEADERS_ACCEPTED);
    BOOST_CHECK_EQUAL(processed.accepted_count, 2U);
    BOOST_CHECK(processed.request_more);
    BOOST_CHECK(!processed.peer_synced);
    BOOST_CHECK(!sync.HasRequestInFlight());

    const auto next_request{sync.NextHeadersRequest()};
    BOOST_CHECK(next_request.locator.vHave.front() == chain.Tip().GetBlockHash());
}

BOOST_AUTO_TEST_CASE(headers_sync_marks_peer_synced_on_short_batch)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    agent::HeaderSyncSession sync{chain, 2};
    const CBlockHeader first{NextHeader(chain)};

    BOOST_REQUIRE(sync.NextHeadersRequest().locator.vHave.front() == Params().GenesisBlock().GetHash());
    const auto processed{sync.ProcessHeaders(std::span{&first, 1})};

    BOOST_CHECK(processed.ok());
    BOOST_CHECK_EQUAL(processed.accepted_count, 1U);
    BOOST_CHECK(!processed.request_more);
    BOOST_CHECK(processed.peer_synced);
    BOOST_CHECK(sync.PeerSynced());
}

BOOST_AUTO_TEST_CASE(headers_sync_marks_peer_synced_on_empty_batch)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    agent::HeaderSyncSession sync{chain, 2};

    BOOST_REQUIRE(sync.NextHeadersRequest().locator.vHave.front() == Params().GenesisBlock().GetHash());
    const auto processed{sync.ProcessHeaders(std::span<const CBlockHeader>{})};

    BOOST_CHECK(processed.ok());
    BOOST_CHECK_EQUAL(processed.code, agent::HeaderSyncResultCode::NO_HEADERS);
    BOOST_CHECK_EQUAL(processed.accepted_count, 0U);
    BOOST_CHECK(processed.peer_synced);
    BOOST_CHECK(sync.PeerSynced());
}

BOOST_AUTO_TEST_CASE(headers_sync_accepts_duplicates_from_superseded_parallel_request)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    agent::HeaderSyncSession first_peer{chain, 2};
    agent::HeaderSyncSession second_peer{chain, 2};
    const CBlockHeader first{NextHeader(chain)};

    BOOST_REQUIRE(first_peer.NextHeadersRequest().locator.vHave.front() == Params().GenesisBlock().GetHash());
    BOOST_REQUIRE(second_peer.NextHeadersRequest().locator.vHave.front() == Params().GenesisBlock().GetHash());
    BOOST_REQUIRE(first_peer.ProcessHeaders(std::span{&first, 1}).ok());

    const auto processed{second_peer.ProcessHeaders(std::span{&first, 1})};

    BOOST_CHECK(processed.ok());
    BOOST_CHECK_EQUAL(processed.code, agent::HeaderSyncResultCode::DUPLICATE_HEADERS);
    BOOST_CHECK_EQUAL(processed.accepted_count, 0U);
    BOOST_CHECK_EQUAL(processed.duplicate_count, 1U);
    BOOST_CHECK(!processed.request_more);
    BOOST_CHECK(processed.peer_synced);
    BOOST_CHECK(second_peer.PeerSynced());
    BOOST_CHECK_EQUAL(chain.Height(), 1);
}

BOOST_AUTO_TEST_CASE(headers_sync_rejects_unrequested_headers)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    agent::HeaderSyncSession sync{chain, 2};
    const CBlockHeader header{NextHeader(chain)};

    const auto processed{sync.ProcessHeaders(std::span{&header, 1})};

    BOOST_CHECK(!processed.ok());
    BOOST_CHECK_EQUAL(processed.code, agent::HeaderSyncResultCode::UNREQUESTED_HEADERS);
    BOOST_CHECK_EQUAL(chain.Height(), 0);
}

BOOST_AUTO_TEST_CASE(headers_sync_rejects_oversized_headers_message)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    agent::HeaderSyncSession sync{chain, 1};
    agent::HeaderChain served_chain{Params().GetConsensus(), Params().GenesisBlock()};
    const CBlockHeader first{NextHeader(served_chain, 0, ArithToUint256(1))};
    BOOST_REQUIRE(served_chain.AcceptHeader(first).accepted());
    const CBlockHeader second{NextHeader(served_chain, 0, ArithToUint256(2))};
    std::vector<CBlockHeader> headers{first, second};

    BOOST_REQUIRE(sync.NextHeadersRequest().locator.vHave.front() == Params().GenesisBlock().GetHash());
    const auto processed{sync.ProcessHeaders(headers)};

    BOOST_CHECK(!processed.ok());
    BOOST_CHECK_EQUAL(processed.code, agent::HeaderSyncResultCode::TOO_MANY_HEADERS);
    BOOST_CHECK_EQUAL(chain.Height(), 0);
    BOOST_CHECK(!sync.HasRequestInFlight());
}

BOOST_AUTO_TEST_CASE(headers_sync_reports_invalid_header)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    agent::HeaderSyncSession sync{chain, 2};
    const CBlockHeader bad{BadPowHeader(chain)};

    BOOST_REQUIRE(sync.NextHeadersRequest().locator.vHave.front() == Params().GenesisBlock().GetHash());
    const auto processed{sync.ProcessHeaders(std::span{&bad, 1})};

    BOOST_CHECK(!processed.ok());
    BOOST_CHECK_EQUAL(processed.code, agent::HeaderSyncResultCode::INVALID_HEADER);
    BOOST_CHECK_EQUAL(processed.header_result.code, agent::HeaderAcceptCode::BAD_POW);
    BOOST_CHECK_EQUAL(chain.Height(), 0);
}

BOOST_AUTO_TEST_CASE(headers_sync_reports_stale_duplicate_response)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    agent::HeaderSyncSession sync{chain, 2};
    const CBlockHeader first{NextHeader(chain)};
    BOOST_REQUIRE(chain.AcceptHeader(first).accepted());

    BOOST_REQUIRE(sync.NextHeadersRequest().locator.vHave.front() == chain.Tip().GetBlockHash());
    const auto processed{sync.ProcessHeaders(std::span{&first, 1})};

    BOOST_CHECK(!processed.ok());
    BOOST_CHECK_EQUAL(processed.code, agent::HeaderSyncResultCode::STALE_HEADERS);
    BOOST_CHECK_EQUAL(processed.duplicate_count, 1U);
    BOOST_CHECK_EQUAL(chain.Height(), 1);
    BOOST_CHECK(!sync.PeerSynced());
}

BOOST_AUTO_TEST_CASE(header_messages_encode_getheaders_request)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    BOOST_REQUIRE(chain.AcceptHeader(NextHeader(chain)).accepted());
    agent::HeaderSyncSession sync{chain};
    const uint256 stop_hash{ArithToUint256(100)};
    const auto request{sync.NextHeadersRequest(stop_hash)};

    const CSerializedNetMsg message{agent::MakeGetHeadersMessage(request)};

    BOOST_CHECK_EQUAL(message.m_type, NetMsgType::GETHEADERS);
    DataStream stream{MakeByteSpan(message.data)};
    CBlockLocator decoded_locator;
    uint256 decoded_stop_hash;
    stream >> decoded_locator >> decoded_stop_hash;
    BOOST_REQUIRE(!decoded_locator.vHave.empty());
    BOOST_CHECK(decoded_locator.vHave.front() == chain.Tip().GetBlockHash());
    BOOST_CHECK(decoded_stop_hash == stop_hash);
    BOOST_CHECK(stream.empty());
}

BOOST_AUTO_TEST_CASE(header_messages_round_trip_headers_payload)
{
    agent::HeaderChain served_chain{Params().GetConsensus(), Params().GenesisBlock()};
    const CBlockHeader first{NextHeader(served_chain, 0, ArithToUint256(1))};
    BOOST_REQUIRE(served_chain.AcceptHeader(first).accepted());
    const CBlockHeader second{NextHeader(served_chain, 0, ArithToUint256(2))};
    std::vector<CBlockHeader> headers{first, second};

    const CSerializedNetMsg message{agent::MakeHeadersMessage(headers)};
    const auto decoded{agent::DecodeHeadersMessage(message, 2)};

    BOOST_REQUIRE(decoded.ok());
    BOOST_CHECK_EQUAL(decoded.announced_count, 2U);
    BOOST_CHECK_EQUAL(decoded.decoded_count, 2U);
    BOOST_REQUIRE_EQUAL(decoded.headers.size(), 2U);
    BOOST_CHECK(decoded.headers[0].GetHash() == first.GetHash());
    BOOST_CHECK(decoded.headers[1].GetHash() == second.GetHash());
}

BOOST_AUTO_TEST_CASE(header_messages_reject_wrong_message_type)
{
    CSerializedNetMsg message{NetMsg::Make(NetMsgType::PING, uint64_t{1})};

    const auto decoded{agent::DecodeHeadersMessage(message, 2)};

    BOOST_CHECK(!decoded.ok());
    BOOST_CHECK_EQUAL(decoded.code, agent::HeaderMessageResultCode::WRONG_MESSAGE_TYPE);
    BOOST_CHECK_EQUAL(agent::HeaderMessageResultCodeString(decoded.code), "wrong-message-type");
}

BOOST_AUTO_TEST_CASE(header_messages_reject_oversized_payload_before_decoding_headers)
{
    agent::HeaderChain served_chain{Params().GetConsensus(), Params().GenesisBlock()};
    const CBlockHeader first{NextHeader(served_chain, 0, ArithToUint256(1))};
    BOOST_REQUIRE(served_chain.AcceptHeader(first).accepted());
    const CBlockHeader second{NextHeader(served_chain, 0, ArithToUint256(2))};
    const CSerializedNetMsg message{agent::MakeHeadersMessage(std::vector<CBlockHeader>{first, second})};

    const auto decoded{agent::DecodeHeadersMessage(message, 1)};

    BOOST_CHECK(!decoded.ok());
    BOOST_CHECK_EQUAL(decoded.code, agent::HeaderMessageResultCode::TOO_MANY_HEADERS);
    BOOST_CHECK_EQUAL(decoded.announced_count, 2U);
    BOOST_CHECK(decoded.headers.empty());
}

BOOST_AUTO_TEST_CASE(header_messages_reject_nonempty_header_tx_count)
{
    agent::HeaderChain served_chain{Params().GetConsensus(), Params().GenesisBlock()};
    const CBlockHeader first{NextHeader(served_chain)};
    CSerializedNetMsg message;
    message.m_type = NetMsgType::HEADERS;
    VectorWriter writer{message.data, 0};
    WriteCompactSize(writer, 1);
    writer << first;
    WriteCompactSize(writer, 1);

    const auto decoded{agent::DecodeHeadersMessage(message, 1)};

    BOOST_CHECK(!decoded.ok());
    BOOST_CHECK_EQUAL(decoded.code, agent::HeaderMessageResultCode::NONEMPTY_HEADER_TX_COUNT);
    BOOST_CHECK_EQUAL(decoded.announced_count, 1U);
    BOOST_CHECK_EQUAL(decoded.decoded_count, 1U);
    BOOST_CHECK(decoded.headers.empty());
}

BOOST_AUTO_TEST_CASE(header_messages_reject_trailing_data)
{
    agent::HeaderChain served_chain{Params().GetConsensus(), Params().GenesisBlock()};
    const CBlockHeader first{NextHeader(served_chain)};
    CSerializedNetMsg message{agent::MakeHeadersMessage(std::span{&first, 1})};
    VectorWriter{message.data, message.data.size(), uint8_t{1}};

    const auto decoded{agent::DecodeHeadersMessage(message, 1)};

    BOOST_CHECK(!decoded.ok());
    BOOST_CHECK_EQUAL(decoded.code, agent::HeaderMessageResultCode::TRAILING_DATA);
    BOOST_CHECK_EQUAL(decoded.announced_count, 1U);
    BOOST_CHECK_EQUAL(decoded.decoded_count, 1U);
    BOOST_REQUIRE_EQUAL(decoded.headers.size(), 1U);
    BOOST_CHECK(decoded.headers.front().GetHash() == first.GetHash());
}

BOOST_AUTO_TEST_CASE(header_messages_reject_truncated_payload)
{
    CSerializedNetMsg message;
    message.m_type = NetMsgType::HEADERS;
    message.data.push_back(1);

    const auto decoded{agent::DecodeHeadersMessage(message, 1)};

    BOOST_CHECK(!decoded.ok());
    BOOST_CHECK_EQUAL(decoded.code, agent::HeaderMessageResultCode::DESERIALIZE_FAILED);
}

BOOST_AUTO_TEST_CASE(agent_message_io_round_trips_serialized_payload)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    agent::HeaderSyncSession sync{chain};
    const agent::HeaderSyncRequest request{sync.NextHeadersRequest(ArithToUint256(99))};
    const CSerializedNetMsg message{agent::MakeGetHeadersMessage(request)};

    const std::string payload_hex{agent::AgentMessagePayloadHex(message)};
    agent::AgentMessageDecodeResult decoded{agent::DecodeAgentMessage(message.m_type, payload_hex)};

    BOOST_REQUIRE(decoded.ok());
    BOOST_CHECK_EQUAL(decoded.code, agent::AgentMessageIoResultCode::DECODED);
    BOOST_CHECK_EQUAL(decoded.message.m_type, NetMsgType::GETHEADERS);
    BOOST_CHECK(decoded.message.data == message.data);
    BOOST_CHECK_EQUAL(agent::AgentMessageIoResultCodeString(decoded.code), "decoded");
}

BOOST_AUTO_TEST_CASE(agent_message_io_rejects_invalid_cli_payloads)
{
    agent::AgentMessageDecodeResult no_type{agent::DecodeAgentMessage("  ", "00")};
    BOOST_CHECK(!no_type.ok());
    BOOST_CHECK_EQUAL(no_type.code, agent::AgentMessageIoResultCode::INVALID_MESSAGE_TYPE);
    BOOST_CHECK_EQUAL(agent::AgentMessageIoResultCodeString(no_type.code), "invalid-message-type");

    agent::AgentMessageDecodeResult long_type{agent::DecodeAgentMessage("headers-too-long", "00")};
    BOOST_CHECK(!long_type.ok());
    BOOST_CHECK_EQUAL(long_type.code, agent::AgentMessageIoResultCode::INVALID_MESSAGE_TYPE);

    agent::AgentMessageDecodeResult invalid_hex{agent::DecodeAgentMessage(NetMsgType::HEADERS, "abc")};
    BOOST_CHECK(!invalid_hex.ok());
    BOOST_CHECK_EQUAL(invalid_hex.code, agent::AgentMessageIoResultCode::INVALID_HEX);
    BOOST_CHECK_EQUAL(agent::AgentMessageIoResultCodeString(invalid_hex.code), "invalid-hex");
}

BOOST_AUTO_TEST_CASE(agent_client_processes_cli_style_header_payload)
{
    const fs::path path{HeaderStorePath("agent_client_cli_headers.dat")};
    agent::AgentClient client{
        Params().GetConsensus(),
        Params().GenesisBlock(),
        agent::AgentClientOptions{.header_store_path = path},
    };
    const CBlockHeader next{NextHeader(client.Headers())};
    CSerializedNetMsg headers_message{agent::MakeHeadersMessage(std::span{&next, 1})};
    agent::AgentMessageDecodeResult decoded{agent::DecodeAgentMessage(NetMsgType::HEADERS, agent::AgentMessagePayloadHex(headers_message))};
    BOOST_REQUIRE(decoded.ok());

    BOOST_REQUIRE(client.StartHeaders().ok());
    BOOST_REQUIRE(client.PopOutboundMessage().has_value());
    const auto processed{client.ProcessMessage(decoded.message)};

    BOOST_CHECK(processed.ok());
    BOOST_CHECK_EQUAL(processed.code, agent::AgentPeerActionCode::HEADER_MESSAGE);
    BOOST_REQUIRE(processed.header_action.has_value());
    BOOST_CHECK_EQUAL(processed.header_action->driver_result.code, agent::HeaderSyncDriverResultCode::PEER_SYNCED);
    BOOST_CHECK_EQUAL(client.HeaderHeight(), 1);
    BOOST_CHECK_EQUAL(client.SaveHeaders(), agent::HeaderStoreResult::OK);

    agent::AgentClient reloaded{
        Params().GetConsensus(),
        Params().GenesisBlock(),
        agent::AgentClientOptions{.header_store_path = path},
    };
    BOOST_CHECK_EQUAL(reloaded.HeaderHeight(), 1);
    BOOST_CHECK(reloaded.HeaderTip().GetBlockHash() == next.GetHash());
}

BOOST_AUTO_TEST_CASE(agent_client_processes_cli_style_transaction_inventory_payload)
{
    agent::AgentClient client{
        Params().GetConsensus(),
        Params().GenesisBlock(),
        agent::AgentClientOptions{.max_tx_inventory = 2},
    };
    const std::vector<CInv> inventory{
        CInv{MSG_TX, ArithToUint256(40)},
        CInv{MSG_WTX, ArithToUint256(41)},
    };
    CSerializedNetMsg inv_message{agent::MakeTxInvMessage(inventory)};
    agent::AgentMessageDecodeResult decoded{agent::DecodeAgentMessage(NetMsgType::INV, agent::AgentMessagePayloadHex(inv_message))};
    BOOST_REQUIRE(decoded.ok());

    const auto processed{client.ProcessMessage(decoded.message)};

    BOOST_CHECK(processed.ok());
    BOOST_CHECK_EQUAL(processed.code, agent::AgentPeerActionCode::TX_MESSAGE);
    BOOST_REQUIRE(processed.tx_action.has_value());
    BOOST_CHECK_EQUAL(processed.tx_action->result.code, agent::TxPeerResultCode::GETDATA_SENT);
    BOOST_REQUIRE(processed.tx_action->result.inventory_message.has_value());
    BOOST_CHECK_EQUAL(processed.tx_action->result.inventory_message->decoded_count, 2U);
    BOOST_CHECK_EQUAL(processed.tx_action->result.requested_inventory.size(), 2U);
    BOOST_CHECK_EQUAL(client.PendingTxRequestCount(), 2U);
    BOOST_CHECK_EQUAL(client.OutboundMessageCount(), 1U);

    const auto getdata_message{client.PopOutboundMessage()};
    BOOST_REQUIRE(getdata_message.has_value());
    BOOST_CHECK_EQUAL(getdata_message->m_type, NetMsgType::GETDATA);
    const auto getdata{agent::DecodeTxGetDataMessage(*getdata_message, 2)};
    BOOST_REQUIRE(getdata.ok());
    BOOST_REQUIRE_EQUAL(getdata.inventory.size(), 2U);
    BOOST_CHECK_EQUAL(getdata.inventory[0].type, MSG_TX);
    BOOST_CHECK(getdata.inventory[0].hash == inventory[0].hash);
    BOOST_CHECK_EQUAL(getdata.inventory[1].type, MSG_WTX);
    BOOST_CHECK(getdata.inventory[1].hash == inventory[1].hash);
}

BOOST_AUTO_TEST_CASE(agent_client_processes_cli_style_transaction_payload)
{
    agent::AgentClient client{Params().GetConsensus(), Params().GenesisBlock()};
    const CTransactionRef transaction{TestTransaction(22)};
    CSerializedNetMsg tx_message{agent::MakeTxMessage(*transaction)};
    agent::AgentMessageDecodeResult decoded{agent::DecodeAgentMessage(NetMsgType::TX, agent::AgentMessagePayloadHex(tx_message))};
    BOOST_REQUIRE(decoded.ok());

    const auto processed{client.ProcessMessage(decoded.message)};

    BOOST_CHECK(processed.ok());
    BOOST_CHECK_EQUAL(processed.code, agent::AgentPeerActionCode::TX_MESSAGE);
    BOOST_REQUIRE(processed.tx_action.has_value());
    BOOST_CHECK_EQUAL(processed.tx_action->result.code, agent::TxPeerResultCode::TX_RECEIVED);
    BOOST_REQUIRE(processed.tx_action->result.tx_message.has_value());
    BOOST_REQUIRE(processed.tx_action->result.tx_message->transaction);
    BOOST_CHECK(processed.tx_action->result.tx_message->transaction->GetHash() == transaction->GetHash());
    BOOST_CHECK(processed.tx_action->result.tx_message->transaction->GetWitnessHash() == transaction->GetWitnessHash());
    BOOST_CHECK_EQUAL(processed.tx_action->result.tx_message->transaction->nAnchorHeight, 22U);
    BOOST_CHECK_EQUAL(processed.tx_action->result.tx_message->transaction->nPowNonce, 99U);
    BOOST_CHECK_EQUAL(processed.tx_action->result.tx_message->transaction->nCycle[0], 7U);
    BOOST_CHECK_EQUAL(processed.tx_action->result.tx_message->transaction->nCycle[41], 42U);
    BOOST_CHECK_EQUAL(client.KnownTxInventoryCount(), 3U);
    BOOST_CHECK(!client.HasOutboundMessages());
}

BOOST_AUTO_TEST_CASE(header_messages_process_decoded_headers_through_sync_session)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    agent::HeaderSyncSession sync{chain, 2};
    agent::HeaderChain served_chain{Params().GetConsensus(), Params().GenesisBlock()};
    const CBlockHeader first{NextHeader(served_chain, 0, ArithToUint256(1))};
    BOOST_REQUIRE(served_chain.AcceptHeader(first).accepted());
    const CBlockHeader second{NextHeader(served_chain, 0, ArithToUint256(2))};
    std::vector<CBlockHeader> headers{first, second};
    const CSerializedNetMsg message{agent::MakeHeadersMessage(headers)};

    BOOST_REQUIRE(sync.NextHeadersRequest().locator.vHave.front() == Params().GenesisBlock().GetHash());
    const auto processed{agent::ProcessHeadersMessage(sync, message, 2)};

    BOOST_REQUIRE(processed.decode.ok());
    BOOST_REQUIRE(processed.sync.has_value());
    BOOST_CHECK(processed.ok());
    BOOST_CHECK_EQUAL(processed.sync->code, agent::HeaderSyncResultCode::HEADERS_ACCEPTED);
    BOOST_CHECK_EQUAL(processed.sync->accepted_count, 2U);
    BOOST_CHECK_EQUAL(chain.Height(), 2);
}

BOOST_AUTO_TEST_CASE(header_messages_decode_failure_does_not_mutate_sync_session)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    agent::HeaderSyncSession sync{chain, 2};
    CSerializedNetMsg message{NetMsg::Make(NetMsgType::PING, uint64_t{1})};

    BOOST_REQUIRE(sync.NextHeadersRequest().locator.vHave.front() == Params().GenesisBlock().GetHash());
    const auto processed{agent::ProcessHeadersMessage(sync, message, 2)};

    BOOST_CHECK(!processed.ok());
    BOOST_CHECK(!processed.decode.ok());
    BOOST_CHECK(!processed.sync.has_value());
    BOOST_CHECK_EQUAL(chain.Height(), 0);
    BOOST_CHECK(sync.HasRequestInFlight());
}

BOOST_AUTO_TEST_CASE(header_driver_sends_getheaders_and_suppresses_duplicate_request)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    BOOST_REQUIRE(chain.AcceptHeader(NextHeader(chain)).accepted());
    agent::HeaderSyncSession sync{chain};
    agent::HeaderSyncDriver driver{sync};
    const uint256 stop_hash{ArithToUint256(77)};

    const auto requested{driver.RequestHeaders(stop_hash)};

    BOOST_REQUIRE(requested.ok());
    BOOST_CHECK_EQUAL(requested.code, agent::HeaderSyncDriverResultCode::GETHEADERS_SENT);
    BOOST_REQUIRE(requested.outbound_message.has_value());
    BOOST_CHECK_EQUAL(requested.outbound_message->m_type, NetMsgType::GETHEADERS);
    BOOST_CHECK(driver.WaitingForHeaders());
    BOOST_CHECK(driver.StopHash() == stop_hash);

    DataStream stream{MakeByteSpan(requested.outbound_message->data)};
    CBlockLocator decoded_locator;
    uint256 decoded_stop_hash;
    stream >> decoded_locator >> decoded_stop_hash;
    BOOST_REQUIRE(!decoded_locator.vHave.empty());
    BOOST_CHECK(decoded_locator.vHave.front() == chain.Tip().GetBlockHash());
    BOOST_CHECK(decoded_stop_hash == stop_hash);
    BOOST_CHECK(stream.empty());

    const uint256 replacement_stop_hash{ArithToUint256(78)};
    const auto duplicate_with_stop{driver.RequestHeaders(replacement_stop_hash)};
    BOOST_CHECK(duplicate_with_stop.ok());
    BOOST_CHECK_EQUAL(duplicate_with_stop.code, agent::HeaderSyncDriverResultCode::WAITING_FOR_HEADERS);
    BOOST_CHECK(driver.StopHash() == stop_hash);

    const auto duplicate{driver.RequestHeaders()};
    BOOST_CHECK(duplicate.ok());
    BOOST_CHECK_EQUAL(duplicate.code, agent::HeaderSyncDriverResultCode::WAITING_FOR_HEADERS);
    BOOST_CHECK(!duplicate.outbound_message.has_value());
}

BOOST_AUTO_TEST_CASE(header_driver_ignores_unrelated_messages_without_mutating_request)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    agent::HeaderSyncSession sync{chain};
    agent::HeaderSyncDriver driver{sync};

    BOOST_REQUIRE(driver.RequestHeaders().outbound_message.has_value());
    const auto ignored{driver.ProcessMessage(NetMsg::Make(NetMsgType::PING, uint64_t{1}))};

    BOOST_CHECK(ignored.ok());
    BOOST_CHECK_EQUAL(ignored.code, agent::HeaderSyncDriverResultCode::IGNORED_MESSAGE);
    BOOST_CHECK(!ignored.outbound_message.has_value());
    BOOST_CHECK(!ignored.header_message.has_value());
    BOOST_CHECK(driver.WaitingForHeaders());
    BOOST_CHECK_EQUAL(chain.Height(), 0);
}

BOOST_AUTO_TEST_CASE(header_driver_processes_full_batch_and_emits_followup_request)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    agent::HeaderSyncSession sync{chain, 2};
    agent::HeaderSyncDriver driver{sync, ArithToUint256(99)};
    agent::HeaderChain served_chain{Params().GetConsensus(), Params().GenesisBlock()};
    const CBlockHeader first{NextHeader(served_chain, 0, ArithToUint256(1))};
    BOOST_REQUIRE(served_chain.AcceptHeader(first).accepted());
    const CBlockHeader second{NextHeader(served_chain, 0, ArithToUint256(2))};
    std::vector<CBlockHeader> headers{first, second};

    BOOST_REQUIRE(driver.RequestHeaders().outbound_message.has_value());
    const auto processed{driver.ProcessMessage(agent::MakeHeadersMessage(headers))};

    BOOST_REQUIRE(processed.ok());
    BOOST_CHECK_EQUAL(processed.code, agent::HeaderSyncDriverResultCode::HEADERS_ACCEPTED);
    BOOST_REQUIRE(processed.header_message.has_value());
    BOOST_REQUIRE(processed.header_message->sync.has_value());
    BOOST_CHECK_EQUAL(processed.header_message->sync->accepted_count, 2U);
    BOOST_CHECK(processed.header_message->sync->request_more);
    BOOST_REQUIRE(processed.outbound_message.has_value());
    BOOST_CHECK_EQUAL(processed.outbound_message->m_type, NetMsgType::GETHEADERS);
    BOOST_CHECK(driver.WaitingForHeaders());
    BOOST_CHECK_EQUAL(chain.Height(), 2);

    DataStream stream{MakeByteSpan(processed.outbound_message->data)};
    CBlockLocator decoded_locator;
    uint256 decoded_stop_hash;
    stream >> decoded_locator >> decoded_stop_hash;
    BOOST_REQUIRE(!decoded_locator.vHave.empty());
    BOOST_CHECK(decoded_locator.vHave.front() == chain.Tip().GetBlockHash());
    BOOST_CHECK(decoded_stop_hash == driver.StopHash());
}

BOOST_AUTO_TEST_CASE(header_driver_marks_peer_synced_on_short_batch)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    agent::HeaderSyncSession sync{chain, 2};
    agent::HeaderSyncDriver driver{sync};
    const CBlockHeader first{NextHeader(chain)};

    BOOST_REQUIRE(driver.RequestHeaders().outbound_message.has_value());
    const auto processed{driver.ProcessMessage(agent::MakeHeadersMessage(std::span{&first, 1}))};

    BOOST_CHECK(processed.ok());
    BOOST_CHECK_EQUAL(processed.code, agent::HeaderSyncDriverResultCode::PEER_SYNCED);
    BOOST_REQUIRE(processed.header_message.has_value());
    BOOST_REQUIRE(processed.header_message->sync.has_value());
    BOOST_CHECK_EQUAL(processed.header_message->sync->accepted_count, 1U);
    BOOST_CHECK(!processed.outbound_message.has_value());
    BOOST_CHECK(!driver.WaitingForHeaders());
    BOOST_CHECK(driver.PeerSynced());
    BOOST_CHECK_EQUAL(chain.Height(), 1);
}

BOOST_AUTO_TEST_CASE(header_driver_reports_decode_failure_without_mutating_request)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    agent::HeaderSyncSession sync{chain, 2};
    agent::HeaderSyncDriver driver{sync};
    CSerializedNetMsg message;
    message.m_type = NetMsgType::HEADERS;
    message.data.push_back(1);

    BOOST_REQUIRE(driver.RequestHeaders().outbound_message.has_value());
    const auto processed{driver.ProcessMessage(message)};

    BOOST_CHECK(!processed.ok());
    BOOST_CHECK_EQUAL(processed.code, agent::HeaderSyncDriverResultCode::DECODE_FAILED);
    BOOST_REQUIRE(processed.header_message.has_value());
    BOOST_CHECK_EQUAL(processed.header_message->decode.code, agent::HeaderMessageResultCode::DESERIALIZE_FAILED);
    BOOST_CHECK(!processed.header_message->sync.has_value());
    BOOST_CHECK(!processed.outbound_message.has_value());
    BOOST_CHECK(driver.WaitingForHeaders());
    BOOST_CHECK_EQUAL(chain.Height(), 0);
}

BOOST_AUTO_TEST_CASE(header_driver_reports_sync_failure_for_stale_headers)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    agent::HeaderSyncSession sync{chain, 2};
    agent::HeaderSyncDriver driver{sync};
    const CBlockHeader first{NextHeader(chain)};
    BOOST_REQUIRE(chain.AcceptHeader(first).accepted());

    BOOST_REQUIRE(driver.RequestHeaders().outbound_message.has_value());
    const auto processed{driver.ProcessMessage(agent::MakeHeadersMessage(std::span{&first, 1}))};

    BOOST_CHECK(!processed.ok());
    BOOST_CHECK_EQUAL(processed.code, agent::HeaderSyncDriverResultCode::SYNC_FAILED);
    BOOST_REQUIRE(processed.header_message.has_value());
    BOOST_REQUIRE(processed.header_message->sync.has_value());
    BOOST_CHECK_EQUAL(processed.header_message->sync->code, agent::HeaderSyncResultCode::STALE_HEADERS);
    BOOST_CHECK(!processed.outbound_message.has_value());
    BOOST_CHECK(!driver.WaitingForHeaders());
    BOOST_CHECK(!driver.PeerSynced());
    BOOST_CHECK_EQUAL(chain.Height(), 1);
}

BOOST_AUTO_TEST_CASE(header_driver_accepts_duplicate_response_from_superseded_parallel_request)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    agent::HeaderSyncSession first_sync{chain, 2};
    agent::HeaderSyncSession second_sync{chain, 2};
    agent::HeaderSyncDriver first_driver{first_sync};
    agent::HeaderSyncDriver second_driver{second_sync};
    const CBlockHeader first{NextHeader(chain)};

    BOOST_REQUIRE(first_driver.RequestHeaders().outbound_message.has_value());
    BOOST_REQUIRE(second_driver.RequestHeaders().outbound_message.has_value());
    BOOST_REQUIRE(first_driver.ProcessMessage(agent::MakeHeadersMessage(std::span{&first, 1})).ok());

    const auto processed{second_driver.ProcessMessage(agent::MakeHeadersMessage(std::span{&first, 1}))};

    BOOST_CHECK(processed.ok());
    BOOST_CHECK_EQUAL(processed.code, agent::HeaderSyncDriverResultCode::PEER_SYNCED);
    BOOST_REQUIRE(processed.header_message.has_value());
    BOOST_REQUIRE(processed.header_message->sync.has_value());
    BOOST_CHECK_EQUAL(processed.header_message->sync->code, agent::HeaderSyncResultCode::DUPLICATE_HEADERS);
    BOOST_CHECK_EQUAL(processed.header_message->sync->duplicate_count, 1U);
    BOOST_CHECK(!processed.outbound_message.has_value());
    BOOST_CHECK(!second_driver.WaitingForHeaders());
    BOOST_CHECK(second_driver.PeerSynced());
    BOOST_CHECK_EQUAL(chain.Height(), 1);
}

BOOST_AUTO_TEST_CASE(header_peer_queues_start_request_for_transport)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    BOOST_REQUIRE(chain.AcceptHeader(NextHeader(chain)).accepted());
    agent::HeaderSyncPeer peer{chain};
    const uint256 stop_hash{ArithToUint256(111)};

    const auto action{peer.StartHeaders(stop_hash)};

    BOOST_CHECK(action.ok());
    BOOST_CHECK_EQUAL(action.driver_result.code, agent::HeaderSyncDriverResultCode::GETHEADERS_SENT);
    BOOST_CHECK(action.queued_outbound_message);
    BOOST_CHECK(!action.driver_result.outbound_message.has_value());
    BOOST_CHECK(peer.WaitingForHeaders());
    BOOST_CHECK(peer.StopHash() == stop_hash);
    BOOST_CHECK_EQUAL(peer.OutboundMessageCount(), 1U);

    const auto message{peer.PopOutboundMessage()};
    BOOST_REQUIRE(message.has_value());
    BOOST_CHECK_EQUAL(message->m_type, NetMsgType::GETHEADERS);
    BOOST_CHECK(!peer.HasOutboundMessages());

    DataStream stream{MakeByteSpan(message->data)};
    CBlockLocator decoded_locator;
    uint256 decoded_stop_hash;
    stream >> decoded_locator >> decoded_stop_hash;
    BOOST_REQUIRE(!decoded_locator.vHave.empty());
    BOOST_CHECK(decoded_locator.vHave.front() == chain.Tip().GetBlockHash());
    BOOST_CHECK(decoded_stop_hash == stop_hash);
    BOOST_CHECK(stream.empty());
}

BOOST_AUTO_TEST_CASE(header_peer_suppresses_duplicate_start_without_queuing)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    agent::HeaderSyncPeer peer{chain};
    const uint256 stop_hash{ArithToUint256(222)};

    BOOST_REQUIRE(peer.StartHeaders(stop_hash).queued_outbound_message);
    const auto duplicate{peer.StartHeaders(ArithToUint256(223))};

    BOOST_CHECK(duplicate.ok());
    BOOST_CHECK_EQUAL(duplicate.driver_result.code, agent::HeaderSyncDriverResultCode::WAITING_FOR_HEADERS);
    BOOST_CHECK(!duplicate.queued_outbound_message);
    BOOST_CHECK(!duplicate.driver_result.outbound_message.has_value());
    BOOST_CHECK(peer.StopHash() == stop_hash);
    BOOST_CHECK_EQUAL(peer.OutboundMessageCount(), 1U);
}

BOOST_AUTO_TEST_CASE(header_peer_queues_followup_request_after_full_batch)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    agent::HeaderSyncPeer peer{chain, ArithToUint256(333), 2};
    agent::HeaderChain served_chain{Params().GetConsensus(), Params().GenesisBlock()};
    const CBlockHeader first{NextHeader(served_chain, 0, ArithToUint256(1))};
    BOOST_REQUIRE(served_chain.AcceptHeader(first).accepted());
    const CBlockHeader second{NextHeader(served_chain, 0, ArithToUint256(2))};
    std::vector<CBlockHeader> headers{first, second};

    BOOST_REQUIRE(peer.StartHeaders().queued_outbound_message);
    BOOST_REQUIRE(peer.PopOutboundMessage().has_value());
    const auto action{peer.ProcessMessage(agent::MakeHeadersMessage(headers))};

    BOOST_CHECK(action.ok());
    BOOST_CHECK_EQUAL(action.driver_result.code, agent::HeaderSyncDriverResultCode::HEADERS_ACCEPTED);
    BOOST_CHECK(action.queued_outbound_message);
    BOOST_CHECK(!action.driver_result.outbound_message.has_value());
    BOOST_CHECK(peer.WaitingForHeaders());
    BOOST_CHECK_EQUAL(chain.Height(), 2);

    const auto messages{peer.DrainOutboundMessages()};
    BOOST_REQUIRE_EQUAL(messages.size(), 1U);
    BOOST_CHECK_EQUAL(messages.front().m_type, NetMsgType::GETHEADERS);

    DataStream stream{MakeByteSpan(messages.front().data)};
    CBlockLocator decoded_locator;
    uint256 decoded_stop_hash;
    stream >> decoded_locator >> decoded_stop_hash;
    BOOST_REQUIRE(!decoded_locator.vHave.empty());
    BOOST_CHECK(decoded_locator.vHave.front() == chain.Tip().GetBlockHash());
    BOOST_CHECK(decoded_stop_hash == peer.StopHash());
}

BOOST_AUTO_TEST_CASE(header_peer_does_not_queue_ignored_or_failed_messages)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    agent::HeaderSyncPeer peer{chain, uint256{}, 2};

    BOOST_REQUIRE(peer.StartHeaders().queued_outbound_message);
    BOOST_REQUIRE(peer.PopOutboundMessage().has_value());

    const auto ignored{peer.ProcessMessage(NetMsg::Make(NetMsgType::PING, uint64_t{1}))};
    BOOST_CHECK(ignored.ok());
    BOOST_CHECK_EQUAL(ignored.driver_result.code, agent::HeaderSyncDriverResultCode::IGNORED_MESSAGE);
    BOOST_CHECK(!ignored.queued_outbound_message);
    BOOST_CHECK(!peer.HasOutboundMessages());
    BOOST_CHECK(peer.WaitingForHeaders());

    CSerializedNetMsg malformed;
    malformed.m_type = NetMsgType::HEADERS;
    malformed.data.push_back(1);
    const auto failed{peer.ProcessMessage(malformed)};

    BOOST_CHECK(!failed.ok());
    BOOST_CHECK_EQUAL(failed.driver_result.code, agent::HeaderSyncDriverResultCode::DECODE_FAILED);
    BOOST_CHECK(!failed.queued_outbound_message);
    BOOST_CHECK(!peer.HasOutboundMessages());
    BOOST_CHECK(peer.WaitingForHeaders());
    BOOST_CHECK_EQUAL(chain.Height(), 0);
}

BOOST_AUTO_TEST_CASE(tx_messages_convert_gentxid_to_transaction_inventory)
{
    const uint256 txid_hash{ArithToUint256(10)};
    const uint256 wtxid_hash{ArithToUint256(11)};
    const std::vector<GenTxid> txids{GenTxid::Txid(txid_hash), GenTxid::Wtxid(wtxid_hash)};

    const auto inventory{agent::TransactionInventory(txids)};

    BOOST_REQUIRE_EQUAL(inventory.size(), 2U);
    BOOST_CHECK_EQUAL(inventory[0].type, MSG_TX);
    BOOST_CHECK(inventory[0].hash == txid_hash);
    BOOST_CHECK_EQUAL(inventory[1].type, MSG_WTX);
    BOOST_CHECK(inventory[1].hash == wtxid_hash);
}

BOOST_AUTO_TEST_CASE(tx_messages_round_trip_inv_payload)
{
    const std::vector<CInv> inventory{
        CInv{MSG_TX, ArithToUint256(1)},
        CInv{MSG_WTX, ArithToUint256(2)},
        CInv{MSG_WITNESS_TX, ArithToUint256(3)},
    };

    const CSerializedNetMsg message{agent::MakeTxInvMessage(inventory)};
    const auto decoded{agent::DecodeTxInvMessage(message)};

    BOOST_REQUIRE(decoded.ok());
    BOOST_CHECK_EQUAL(decoded.announced_count, 3U);
    BOOST_CHECK_EQUAL(decoded.decoded_count, 3U);
    BOOST_REQUIRE_EQUAL(decoded.inventory.size(), 3U);
    BOOST_CHECK_EQUAL(decoded.inventory[0].type, MSG_TX);
    BOOST_CHECK(decoded.inventory[0].hash == inventory[0].hash);
    BOOST_CHECK_EQUAL(decoded.inventory[1].type, MSG_WTX);
    BOOST_CHECK(decoded.inventory[1].hash == inventory[1].hash);
    BOOST_CHECK_EQUAL(decoded.inventory[2].type, MSG_WITNESS_TX);
    BOOST_CHECK(decoded.inventory[2].hash == inventory[2].hash);
}

BOOST_AUTO_TEST_CASE(tx_messages_round_trip_getdata_payload)
{
    const std::vector<CInv> inventory{
        CInv{MSG_TX, ArithToUint256(20)},
        CInv{MSG_WTX, ArithToUint256(21)},
    };

    const CSerializedNetMsg message{agent::MakeTxGetDataMessage(inventory)};
    const auto decoded{agent::DecodeTxGetDataMessage(message)};

    BOOST_REQUIRE(decoded.ok());
    BOOST_CHECK_EQUAL(message.m_type, NetMsgType::GETDATA);
    BOOST_CHECK_EQUAL(decoded.announced_count, 2U);
    BOOST_REQUIRE_EQUAL(decoded.inventory.size(), 2U);
    BOOST_CHECK_EQUAL(decoded.inventory[0].type, MSG_TX);
    BOOST_CHECK(decoded.inventory[0].hash == inventory[0].hash);
    BOOST_CHECK_EQUAL(decoded.inventory[1].type, MSG_WTX);
    BOOST_CHECK(decoded.inventory[1].hash == inventory[1].hash);
}

BOOST_AUTO_TEST_CASE(tx_messages_reject_wrong_inventory_message_type)
{
    const auto decoded{agent::DecodeTxInvMessage(NetMsg::Make(NetMsgType::PING, uint64_t{1}))};

    BOOST_CHECK(!decoded.ok());
    BOOST_CHECK_EQUAL(decoded.code, agent::TxMessageResultCode::WRONG_MESSAGE_TYPE);
    BOOST_CHECK_EQUAL(agent::TxMessageResultCodeString(decoded.code), "wrong-message-type");
}

BOOST_AUTO_TEST_CASE(tx_messages_reject_oversized_inventory_before_decoding_entries)
{
    const std::vector<CInv> inventory{
        CInv{MSG_TX, ArithToUint256(1)},
        CInv{MSG_TX, ArithToUint256(2)},
    };

    const auto decoded{agent::DecodeTxInvMessage(agent::MakeTxInvMessage(inventory), 1)};

    BOOST_CHECK(!decoded.ok());
    BOOST_CHECK_EQUAL(decoded.code, agent::TxMessageResultCode::TOO_MANY_INVENTORY);
    BOOST_CHECK_EQUAL(agent::TxMessageResultCodeString(decoded.code), "too-many-inventory");
    BOOST_CHECK_EQUAL(decoded.announced_count, 2U);
    BOOST_CHECK(decoded.inventory.empty());
}

BOOST_AUTO_TEST_CASE(tx_messages_reject_non_transaction_inventory)
{
    const std::vector<CInv> inventory{
        CInv{MSG_TX, ArithToUint256(1)},
        CInv{MSG_BLOCK, ArithToUint256(2)},
    };

    const auto decoded{agent::DecodeTxInvMessage(agent::MakeTxInvMessage(inventory))};

    BOOST_CHECK(!decoded.ok());
    BOOST_CHECK_EQUAL(decoded.code, agent::TxMessageResultCode::NON_TRANSACTION_INVENTORY);
    BOOST_CHECK_EQUAL(agent::TxMessageResultCodeString(decoded.code), "non-transaction-inventory");
    BOOST_CHECK_EQUAL(decoded.announced_count, 2U);
    BOOST_CHECK_EQUAL(decoded.decoded_count, 2U);
    BOOST_REQUIRE_EQUAL(decoded.inventory.size(), 1U);
    BOOST_CHECK_EQUAL(decoded.inventory.front().type, MSG_TX);
}

BOOST_AUTO_TEST_CASE(tx_messages_reject_inventory_trailing_data)
{
    CSerializedNetMsg message{agent::MakeTxInvMessage(std::vector<CInv>{CInv{MSG_TX, ArithToUint256(1)}})};
    VectorWriter{message.data, message.data.size(), uint8_t{1}};

    const auto decoded{agent::DecodeTxInvMessage(message)};

    BOOST_CHECK(!decoded.ok());
    BOOST_CHECK_EQUAL(decoded.code, agent::TxMessageResultCode::TRAILING_DATA);
    BOOST_CHECK_EQUAL(decoded.announced_count, 1U);
    BOOST_CHECK_EQUAL(decoded.decoded_count, 1U);
    BOOST_REQUIRE_EQUAL(decoded.inventory.size(), 1U);
    BOOST_CHECK_EQUAL(decoded.inventory.front().type, MSG_TX);
}

BOOST_AUTO_TEST_CASE(tx_messages_reject_truncated_inventory)
{
    CSerializedNetMsg message;
    message.m_type = NetMsgType::INV;
    message.data.push_back(1);

    const auto decoded{agent::DecodeTxInvMessage(message)};

    BOOST_CHECK(!decoded.ok());
    BOOST_CHECK_EQUAL(decoded.code, agent::TxMessageResultCode::DESERIALIZE_FAILED);
}

BOOST_AUTO_TEST_CASE(tx_messages_round_trip_tx_payload_with_quicksilver_pow_tail)
{
    const CTransactionRef transaction{TestTransaction()};

    const CSerializedNetMsg message{agent::MakeTxMessage(*transaction)};
    const auto decoded{agent::DecodeTxMessage(message)};

    BOOST_REQUIRE(decoded.ok());
    BOOST_REQUIRE(decoded.transaction);
    BOOST_CHECK(decoded.transaction->GetHash() == transaction->GetHash());
    BOOST_CHECK(decoded.transaction->GetWitnessHash() == transaction->GetWitnessHash());
    BOOST_CHECK_EQUAL(decoded.transaction->nAnchorHeight, 10U);
    BOOST_CHECK_EQUAL(decoded.transaction->nPowNonce, 99U);
    BOOST_CHECK_EQUAL(decoded.transaction->nCycle[0], 7U);
    BOOST_CHECK_EQUAL(decoded.transaction->nCycle[41], 42U);
    BOOST_REQUIRE_EQUAL(decoded.transaction->vin.size(), 1U);
    BOOST_CHECK_EQUAL(decoded.transaction->vin.front().scriptWitness.stack.size(), 1U);
}

BOOST_AUTO_TEST_CASE(tx_messages_reject_wrong_tx_message_type)
{
    const auto decoded{agent::DecodeTxMessage(NetMsg::Make(NetMsgType::PING, uint64_t{1}))};

    BOOST_CHECK(!decoded.ok());
    BOOST_CHECK_EQUAL(decoded.code, agent::TxMessageResultCode::WRONG_MESSAGE_TYPE);
}

BOOST_AUTO_TEST_CASE(tx_messages_reject_tx_trailing_data)
{
    CSerializedNetMsg message{agent::MakeTxMessage(*TestTransaction())};
    VectorWriter{message.data, message.data.size(), uint8_t{1}};

    const auto decoded{agent::DecodeTxMessage(message)};

    BOOST_CHECK(!decoded.ok());
    BOOST_CHECK_EQUAL(decoded.code, agent::TxMessageResultCode::TRAILING_DATA);
    BOOST_REQUIRE(decoded.transaction);
    BOOST_CHECK_EQUAL(decoded.transaction->nAnchorHeight, 10U);
}

BOOST_AUTO_TEST_CASE(tx_messages_reject_truncated_tx)
{
    CSerializedNetMsg message;
    message.m_type = NetMsgType::TX;
    message.data.push_back(1);

    const auto decoded{agent::DecodeTxMessage(message)};

    BOOST_CHECK(!decoded.ok());
    BOOST_CHECK_EQUAL(decoded.code, agent::TxMessageResultCode::DESERIALIZE_FAILED);
}

BOOST_AUTO_TEST_CASE(tx_peer_queues_getdata_for_new_inventory)
{
    agent::TxPeer peer{2};
    const std::vector<CInv> inventory{
        CInv{MSG_TX, ArithToUint256(1)},
        CInv{MSG_WTX, ArithToUint256(2)},
    };

    const auto action{peer.ProcessMessage(agent::MakeTxInvMessage(inventory))};

    BOOST_CHECK(action.ok());
    BOOST_CHECK_EQUAL(action.result.code, agent::TxPeerResultCode::GETDATA_SENT);
    BOOST_CHECK_EQUAL(agent::TxPeerResultCodeString(action.result.code), "getdata-sent");
    BOOST_CHECK(action.queued_outbound_message);
    BOOST_REQUIRE(action.result.inventory_message.has_value());
    BOOST_CHECK(action.result.inventory_message->ok());
    BOOST_CHECK_EQUAL(action.result.duplicate_count, 0U);
    BOOST_REQUIRE_EQUAL(action.result.requested_inventory.size(), 2U);
    BOOST_CHECK_EQUAL(peer.PendingRequestCount(), 2U);
    BOOST_CHECK_EQUAL(peer.OutboundMessageCount(), 1U);

    const auto message{peer.PopOutboundMessage()};
    BOOST_REQUIRE(message.has_value());
    BOOST_CHECK_EQUAL(message->m_type, NetMsgType::GETDATA);
    const auto decoded{agent::DecodeTxGetDataMessage(*message, 2)};
    BOOST_REQUIRE(decoded.ok());
    BOOST_REQUIRE_EQUAL(decoded.inventory.size(), 2U);
    BOOST_CHECK_EQUAL(decoded.inventory[0].type, inventory[0].type);
    BOOST_CHECK(decoded.inventory[0].hash == inventory[0].hash);
    BOOST_CHECK_EQUAL(decoded.inventory[1].type, inventory[1].type);
    BOOST_CHECK(decoded.inventory[1].hash == inventory[1].hash);
    BOOST_CHECK(!peer.HasOutboundMessages());
}

BOOST_AUTO_TEST_CASE(tx_peer_dedupes_pending_inventory)
{
    agent::TxPeer peer;
    const std::vector<CInv> inventory{CInv{MSG_TX, ArithToUint256(10)}};

    BOOST_REQUIRE(peer.ProcessMessage(agent::MakeTxInvMessage(inventory)).queued_outbound_message);
    BOOST_REQUIRE(peer.PopOutboundMessage().has_value());
    const auto duplicate{peer.ProcessMessage(agent::MakeTxInvMessage(inventory))};

    BOOST_CHECK(duplicate.ok());
    BOOST_CHECK_EQUAL(duplicate.result.code, agent::TxPeerResultCode::INV_ALREADY_KNOWN);
    BOOST_CHECK_EQUAL(agent::TxPeerResultCodeString(duplicate.result.code), "inv-already-known");
    BOOST_CHECK(!duplicate.queued_outbound_message);
    BOOST_REQUIRE(duplicate.result.inventory_message.has_value());
    BOOST_CHECK(duplicate.result.inventory_message->ok());
    BOOST_CHECK(duplicate.result.requested_inventory.empty());
    BOOST_CHECK_EQUAL(duplicate.result.duplicate_count, 1U);
    BOOST_CHECK_EQUAL(peer.PendingRequestCount(), 1U);
    BOOST_CHECK(!peer.HasOutboundMessages());
}

BOOST_AUTO_TEST_CASE(tx_peer_surfaces_received_tx_and_marks_inventory_known)
{
    agent::TxPeer peer;
    const CTransactionRef transaction{TestTransaction()};
    const std::vector<CInv> inventory{CInv{MSG_TX, transaction->GetHash()}};

    BOOST_REQUIRE(peer.ProcessMessage(agent::MakeTxInvMessage(inventory)).queued_outbound_message);
    BOOST_REQUIRE(peer.PopOutboundMessage().has_value());
    const auto received{peer.ProcessMessage(agent::MakeTxMessage(*transaction))};

    BOOST_CHECK(received.ok());
    BOOST_CHECK_EQUAL(received.result.code, agent::TxPeerResultCode::TX_RECEIVED);
    BOOST_CHECK_EQUAL(agent::TxPeerResultCodeString(received.result.code), "tx-received");
    BOOST_CHECK(!received.queued_outbound_message);
    BOOST_REQUIRE(received.result.tx_message.has_value());
    BOOST_REQUIRE(received.result.tx_message->ok());
    BOOST_REQUIRE(received.result.tx_message->transaction);
    BOOST_CHECK(received.result.tx_message->transaction->GetHash() == transaction->GetHash());
    BOOST_REQUIRE_EQUAL(received.result.matched_requests.size(), 1U);
    BOOST_CHECK_EQUAL(received.result.matched_requests.front().type, MSG_TX);
    BOOST_CHECK_EQUAL(peer.PendingRequestCount(), 0U);
    BOOST_CHECK_EQUAL(peer.KnownInventoryCount(), 3U);

    const auto duplicate{peer.ProcessMessage(agent::MakeTxInvMessage(inventory))};
    BOOST_CHECK(duplicate.ok());
    BOOST_CHECK_EQUAL(duplicate.result.code, agent::TxPeerResultCode::INV_ALREADY_KNOWN);
    BOOST_CHECK(!duplicate.queued_outbound_message);
    BOOST_CHECK_EQUAL(duplicate.result.duplicate_count, 1U);
    BOOST_CHECK(!peer.HasOutboundMessages());
}

BOOST_AUTO_TEST_CASE(tx_peer_ignores_non_transaction_messages)
{
    agent::TxPeer peer;

    const auto ignored{peer.ProcessMessage(NetMsg::Make(NetMsgType::PING, uint64_t{1}))};

    BOOST_CHECK(ignored.ok());
    BOOST_CHECK_EQUAL(ignored.result.code, agent::TxPeerResultCode::IGNORED_MESSAGE);
    BOOST_CHECK_EQUAL(agent::TxPeerResultCodeString(ignored.result.code), "ignored-message");
    BOOST_CHECK(!ignored.queued_outbound_message);
    BOOST_CHECK(!peer.HasOutboundMessages());
    BOOST_CHECK_EQUAL(peer.PendingRequestCount(), 0U);
}

BOOST_AUTO_TEST_CASE(tx_peer_decode_failures_do_not_queue_or_mutate)
{
    agent::TxPeer peer;
    const std::vector<CInv> inventory{CInv{MSG_BLOCK, ArithToUint256(1)}};

    const auto bad_inv{peer.ProcessMessage(agent::MakeTxInvMessage(inventory))};

    BOOST_CHECK(!bad_inv.ok());
    BOOST_CHECK_EQUAL(bad_inv.result.code, agent::TxPeerResultCode::DECODE_FAILED);
    BOOST_CHECK_EQUAL(agent::TxPeerResultCodeString(bad_inv.result.code), "decode-failed");
    BOOST_CHECK(!bad_inv.queued_outbound_message);
    BOOST_REQUIRE(bad_inv.result.inventory_message.has_value());
    BOOST_CHECK(!bad_inv.result.inventory_message->ok());
    BOOST_CHECK_EQUAL(bad_inv.result.inventory_message->code, agent::TxMessageResultCode::NON_TRANSACTION_INVENTORY);
    BOOST_CHECK(!peer.HasOutboundMessages());
    BOOST_CHECK_EQUAL(peer.PendingRequestCount(), 0U);

    CSerializedNetMsg bad_tx;
    bad_tx.m_type = NetMsgType::TX;
    bad_tx.data.push_back(1);
    const auto malformed_tx{peer.ProcessMessage(bad_tx)};

    BOOST_CHECK(!malformed_tx.ok());
    BOOST_CHECK_EQUAL(malformed_tx.result.code, agent::TxPeerResultCode::DECODE_FAILED);
    BOOST_CHECK(!malformed_tx.queued_outbound_message);
    BOOST_REQUIRE(malformed_tx.result.tx_message.has_value());
    BOOST_CHECK(!malformed_tx.result.tx_message->ok());
    BOOST_CHECK_EQUAL(malformed_tx.result.tx_message->code, agent::TxMessageResultCode::DESERIALIZE_FAILED);
    BOOST_CHECK(!peer.HasOutboundMessages());
    BOOST_CHECK_EQUAL(peer.PendingRequestCount(), 0U);
}

BOOST_AUTO_TEST_CASE(tx_peer_announces_local_transaction_and_answers_getdata)
{
    agent::TxPeer peer;
    const CTransactionRef transaction{TestTransaction()};
    const CInv announced_inv{MSG_WTX, transaction->GetWitnessHash()};

    const auto announced{peer.AnnounceTransaction(*transaction)};

    BOOST_CHECK(announced.ok());
    BOOST_CHECK_EQUAL(announced.result.code, agent::TxPeerResultCode::INV_SENT);
    BOOST_CHECK_EQUAL(agent::TxPeerResultCodeString(announced.result.code), "inv-sent");
    BOOST_CHECK(announced.queued_outbound_message);
    BOOST_REQUIRE_EQUAL(announced.result.announced_inventory.size(), 1U);
    BOOST_CHECK_EQUAL(announced.result.announced_inventory.front().type, MSG_WTX);
    BOOST_CHECK(announced.result.announced_inventory.front().hash == transaction->GetWitnessHash());
    BOOST_CHECK_EQUAL(peer.KnownInventoryCount(), 3U);
    BOOST_CHECK_EQUAL(peer.OutboundMessageCount(), 1U);

    const auto inv_message{peer.PopOutboundMessage()};
    BOOST_REQUIRE(inv_message.has_value());
    BOOST_CHECK_EQUAL(inv_message->m_type, NetMsgType::INV);
    const auto decoded_inv{agent::DecodeTxInvMessage(*inv_message)};
    BOOST_REQUIRE(decoded_inv.ok());
    BOOST_REQUIRE_EQUAL(decoded_inv.inventory.size(), 1U);
    BOOST_CHECK_EQUAL(decoded_inv.inventory.front().type, MSG_WTX);
    BOOST_CHECK(decoded_inv.inventory.front().hash == transaction->GetWitnessHash());

    const auto sent{peer.ProcessMessage(agent::MakeTxGetDataMessage(std::span{&announced_inv, 1}))};

    BOOST_CHECK(sent.ok());
    BOOST_CHECK_EQUAL(sent.result.code, agent::TxPeerResultCode::TX_SENT);
    BOOST_CHECK_EQUAL(agent::TxPeerResultCodeString(sent.result.code), "tx-sent");
    BOOST_CHECK(sent.queued_outbound_message);
    BOOST_REQUIRE(sent.result.inventory_message.has_value());
    BOOST_CHECK(sent.result.inventory_message->ok());
    BOOST_REQUIRE_EQUAL(sent.result.served_inventory.size(), 1U);
    BOOST_CHECK_EQUAL(sent.result.served_inventory.front().type, MSG_WTX);
    BOOST_CHECK_EQUAL(sent.result.missing_count, 0U);
    BOOST_CHECK_EQUAL(peer.OutboundMessageCount(), 1U);

    const auto tx_message{peer.PopOutboundMessage()};
    BOOST_REQUIRE(tx_message.has_value());
    BOOST_CHECK_EQUAL(tx_message->m_type, NetMsgType::TX);
    const auto decoded_tx{agent::DecodeTxMessage(*tx_message)};
    BOOST_REQUIRE(decoded_tx.ok());
    BOOST_REQUIRE(decoded_tx.transaction);
    BOOST_CHECK(decoded_tx.transaction->GetHash() == transaction->GetHash());
    BOOST_CHECK_EQUAL(decoded_tx.transaction->nPowNonce, 99U);
    BOOST_CHECK_EQUAL(decoded_tx.transaction->nCycle[41], 42U);

    const auto duplicate_inv{peer.ProcessMessage(agent::MakeTxInvMessage(std::span{&announced_inv, 1}))};
    BOOST_CHECK(duplicate_inv.ok());
    BOOST_CHECK_EQUAL(duplicate_inv.result.code, agent::TxPeerResultCode::INV_ALREADY_KNOWN);
    BOOST_CHECK(!duplicate_inv.queued_outbound_message);
    BOOST_CHECK_EQUAL(duplicate_inv.result.duplicate_count, 1U);
    BOOST_CHECK(!peer.HasOutboundMessages());
}

BOOST_AUTO_TEST_CASE(tx_peer_reports_missing_getdata_without_queueing_tx)
{
    agent::TxPeer peer;
    const CInv unknown_inv{MSG_TX, ArithToUint256(909)};

    const auto missing{peer.ProcessMessage(agent::MakeTxGetDataMessage(std::span{&unknown_inv, 1}))};

    BOOST_CHECK(missing.ok());
    BOOST_CHECK_EQUAL(missing.result.code, agent::TxPeerResultCode::TX_NOT_FOUND);
    BOOST_CHECK_EQUAL(agent::TxPeerResultCodeString(missing.result.code), "tx-not-found");
    BOOST_CHECK(!missing.queued_outbound_message);
    BOOST_REQUIRE(missing.result.inventory_message.has_value());
    BOOST_CHECK(missing.result.inventory_message->ok());
    BOOST_CHECK(missing.result.served_inventory.empty());
    BOOST_CHECK_EQUAL(missing.result.missing_count, 1U);
    BOOST_CHECK(!peer.HasOutboundMessages());
    BOOST_CHECK_EQUAL(peer.KnownInventoryCount(), 0U);
}

BOOST_AUTO_TEST_CASE(tx_peer_can_send_local_transaction_directly)
{
    agent::TxPeer peer;
    const CTransactionRef transaction{TestTransaction()};

    const auto sent{peer.SendTransaction(*transaction)};

    BOOST_CHECK(sent.ok());
    BOOST_CHECK_EQUAL(sent.result.code, agent::TxPeerResultCode::TX_SENT);
    BOOST_CHECK(sent.queued_outbound_message);
    BOOST_REQUIRE_EQUAL(sent.result.served_inventory.size(), 1U);
    BOOST_CHECK_EQUAL(sent.result.served_inventory.front().type, MSG_WTX);
    BOOST_CHECK(sent.result.served_inventory.front().hash == transaction->GetWitnessHash());
    BOOST_CHECK_EQUAL(peer.KnownInventoryCount(), 3U);

    const auto tx_message{peer.PopOutboundMessage()};
    BOOST_REQUIRE(tx_message.has_value());
    BOOST_CHECK_EQUAL(tx_message->m_type, NetMsgType::TX);
    const auto decoded_tx{agent::DecodeTxMessage(*tx_message)};
    BOOST_REQUIRE(decoded_tx.ok());
    BOOST_REQUIRE(decoded_tx.transaction);
    BOOST_CHECK(decoded_tx.transaction->GetHash() == transaction->GetHash());
    BOOST_CHECK(!peer.HasOutboundMessages());
}

BOOST_AUTO_TEST_CASE(agent_peer_starts_headers_and_exposes_unified_outbound_queue)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    BOOST_REQUIRE(chain.AcceptHeader(NextHeader(chain)).accepted());
    agent::AgentPeer peer{chain};
    const uint256 stop_hash{ArithToUint256(123)};

    const auto action{peer.StartHeaders(stop_hash)};

    BOOST_CHECK(action.ok());
    BOOST_CHECK_EQUAL(action.code, agent::AgentPeerActionCode::HEADERS_STARTED);
    BOOST_CHECK_EQUAL(agent::AgentPeerActionCodeString(action.code), "headers-started");
    BOOST_REQUIRE(action.header_action.has_value());
    BOOST_CHECK(!action.tx_action.has_value());
    BOOST_CHECK_EQUAL(action.header_action->driver_result.code, agent::HeaderSyncDriverResultCode::GETHEADERS_SENT);
    BOOST_CHECK_EQUAL(action.queued_outbound_count, 1U);
    BOOST_CHECK(peer.WaitingForHeaders());
    BOOST_CHECK(peer.HeaderStopHash() == stop_hash);
    BOOST_CHECK_EQUAL(peer.OutboundMessageCount(), 1U);

    const auto message{peer.PopOutboundMessage()};
    BOOST_REQUIRE(message.has_value());
    BOOST_CHECK_EQUAL(message->m_type, NetMsgType::GETHEADERS);
    BOOST_CHECK(!peer.HasOutboundMessages());

    DataStream stream{MakeByteSpan(message->data)};
    CBlockLocator decoded_locator;
    uint256 decoded_stop_hash;
    stream >> decoded_locator >> decoded_stop_hash;
    BOOST_REQUIRE(!decoded_locator.vHave.empty());
    BOOST_CHECK(decoded_locator.vHave.front() == chain.Tip().GetBlockHash());
    BOOST_CHECK(decoded_stop_hash == stop_hash);
    BOOST_CHECK(stream.empty());
}

BOOST_AUTO_TEST_CASE(agent_peer_dispatches_header_messages_to_header_sync)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    agent::AgentPeer peer{chain, uint256{}, 2};
    const CBlockHeader first{NextHeader(chain)};

    BOOST_REQUIRE(peer.StartHeaders().queued_outbound_count == 1U);
    BOOST_REQUIRE(peer.PopOutboundMessage().has_value());
    const auto action{peer.ProcessMessage(agent::MakeHeadersMessage(std::span{&first, 1}))};

    BOOST_CHECK(action.ok());
    BOOST_CHECK_EQUAL(action.code, agent::AgentPeerActionCode::HEADER_MESSAGE);
    BOOST_CHECK_EQUAL(agent::AgentPeerActionCodeString(action.code), "header-message");
    BOOST_REQUIRE(action.header_action.has_value());
    BOOST_CHECK(!action.tx_action.has_value());
    BOOST_CHECK_EQUAL(action.header_action->driver_result.code, agent::HeaderSyncDriverResultCode::PEER_SYNCED);
    BOOST_CHECK_EQUAL(action.queued_outbound_count, 0U);
    BOOST_CHECK(!peer.WaitingForHeaders());
    BOOST_CHECK(peer.HeaderPeerSynced());
    BOOST_CHECK_EQUAL(chain.Height(), 1);
    BOOST_CHECK_EQUAL(peer.PendingTxRequestCount(), 0U);
    BOOST_CHECK(!peer.HasOutboundMessages());
}

BOOST_AUTO_TEST_CASE(agent_peer_dispatches_transaction_messages_to_tx_peer)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    agent::AgentPeer peer{chain};
    const CTransactionRef transaction{TestTransaction()};
    const std::vector<CInv> inventory{CInv{MSG_WTX, transaction->GetWitnessHash()}};

    const auto inv_action{peer.ProcessMessage(agent::MakeTxInvMessage(inventory))};

    BOOST_CHECK(inv_action.ok());
    BOOST_CHECK_EQUAL(inv_action.code, agent::AgentPeerActionCode::TX_MESSAGE);
    BOOST_CHECK_EQUAL(agent::AgentPeerActionCodeString(inv_action.code), "tx-message");
    BOOST_REQUIRE(inv_action.tx_action.has_value());
    BOOST_CHECK(!inv_action.header_action.has_value());
    BOOST_CHECK_EQUAL(inv_action.tx_action->result.code, agent::TxPeerResultCode::GETDATA_SENT);
    BOOST_CHECK_EQUAL(inv_action.queued_outbound_count, 1U);
    BOOST_CHECK_EQUAL(peer.PendingTxRequestCount(), 1U);
    BOOST_CHECK_EQUAL(peer.OutboundMessageCount(), 1U);

    const auto getdata{peer.PopOutboundMessage()};
    BOOST_REQUIRE(getdata.has_value());
    BOOST_CHECK_EQUAL(getdata->m_type, NetMsgType::GETDATA);
    const auto decoded_getdata{agent::DecodeTxGetDataMessage(*getdata)};
    BOOST_REQUIRE(decoded_getdata.ok());
    BOOST_REQUIRE_EQUAL(decoded_getdata.inventory.size(), 1U);
    BOOST_CHECK_EQUAL(decoded_getdata.inventory.front().type, MSG_WTX);
    BOOST_CHECK(decoded_getdata.inventory.front().hash == transaction->GetWitnessHash());

    const auto tx_action{peer.ProcessMessage(agent::MakeTxMessage(*transaction))};

    BOOST_CHECK(tx_action.ok());
    BOOST_CHECK_EQUAL(tx_action.code, agent::AgentPeerActionCode::TX_MESSAGE);
    BOOST_REQUIRE(tx_action.tx_action.has_value());
    BOOST_CHECK_EQUAL(tx_action.tx_action->result.code, agent::TxPeerResultCode::TX_RECEIVED);
    BOOST_REQUIRE(tx_action.tx_action->result.tx_message.has_value());
    BOOST_REQUIRE(tx_action.tx_action->result.tx_message->transaction);
    BOOST_CHECK(tx_action.tx_action->result.tx_message->transaction->GetHash() == transaction->GetHash());
    BOOST_REQUIRE_EQUAL(tx_action.tx_action->result.matched_requests.size(), 1U);
    BOOST_CHECK_EQUAL(tx_action.tx_action->result.matched_requests.front().type, MSG_WTX);
    BOOST_CHECK_EQUAL(tx_action.queued_outbound_count, 0U);
    BOOST_CHECK_EQUAL(peer.PendingTxRequestCount(), 0U);
    BOOST_CHECK_EQUAL(peer.KnownTxInventoryCount(), 3U);
    BOOST_CHECK(!peer.HasOutboundMessages());
}

BOOST_AUTO_TEST_CASE(agent_peer_announces_local_transaction_and_serves_getdata)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    agent::AgentPeer peer{chain};
    const CTransactionRef transaction{TestTransaction()};
    const CInv requested_inv{MSG_WTX, transaction->GetWitnessHash()};

    const auto announced{peer.AnnounceTransaction(*transaction)};

    BOOST_CHECK(announced.ok());
    BOOST_CHECK_EQUAL(announced.code, agent::AgentPeerActionCode::TRANSACTION_ANNOUNCED);
    BOOST_CHECK_EQUAL(agent::AgentPeerActionCodeString(announced.code), "transaction-announced");
    BOOST_REQUIRE(announced.tx_action.has_value());
    BOOST_CHECK(!announced.header_action.has_value());
    BOOST_CHECK_EQUAL(announced.tx_action->result.code, agent::TxPeerResultCode::INV_SENT);
    BOOST_CHECK_EQUAL(announced.queued_outbound_count, 1U);
    BOOST_CHECK_EQUAL(peer.KnownTxInventoryCount(), 3U);
    BOOST_CHECK_EQUAL(peer.OutboundMessageCount(), 1U);

    const auto inv_message{peer.PopOutboundMessage()};
    BOOST_REQUIRE(inv_message.has_value());
    BOOST_CHECK_EQUAL(inv_message->m_type, NetMsgType::INV);
    const auto decoded_inv{agent::DecodeTxInvMessage(*inv_message)};
    BOOST_REQUIRE(decoded_inv.ok());
    BOOST_REQUIRE_EQUAL(decoded_inv.inventory.size(), 1U);
    BOOST_CHECK_EQUAL(decoded_inv.inventory.front().type, MSG_WTX);
    BOOST_CHECK(decoded_inv.inventory.front().hash == transaction->GetWitnessHash());

    const auto served{peer.ProcessMessage(agent::MakeTxGetDataMessage(std::span{&requested_inv, 1}))};

    BOOST_CHECK(served.ok());
    BOOST_CHECK_EQUAL(served.code, agent::AgentPeerActionCode::TX_MESSAGE);
    BOOST_REQUIRE(served.tx_action.has_value());
    BOOST_CHECK_EQUAL(served.tx_action->result.code, agent::TxPeerResultCode::TX_SENT);
    BOOST_REQUIRE_EQUAL(served.tx_action->result.served_inventory.size(), 1U);
    BOOST_CHECK_EQUAL(served.tx_action->result.served_inventory.front().type, MSG_WTX);
    BOOST_CHECK_EQUAL(served.queued_outbound_count, 1U);
    BOOST_CHECK_EQUAL(peer.OutboundMessageCount(), 1U);

    const auto tx_message{peer.PopOutboundMessage()};
    BOOST_REQUIRE(tx_message.has_value());
    BOOST_CHECK_EQUAL(tx_message->m_type, NetMsgType::TX);
    const auto decoded_tx{agent::DecodeTxMessage(*tx_message)};
    BOOST_REQUIRE(decoded_tx.ok());
    BOOST_REQUIRE(decoded_tx.transaction);
    BOOST_CHECK(decoded_tx.transaction->GetWitnessHash() == transaction->GetWitnessHash());
    BOOST_CHECK(!peer.HasOutboundMessages());
}

BOOST_AUTO_TEST_CASE(agent_peer_can_queue_direct_local_transaction_send)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    agent::AgentPeer peer{chain};
    const CTransactionRef transaction{TestTransaction()};

    const auto sent{peer.SendTransaction(*transaction)};

    BOOST_CHECK(sent.ok());
    BOOST_CHECK_EQUAL(sent.code, agent::AgentPeerActionCode::TRANSACTION_SENT);
    BOOST_CHECK_EQUAL(agent::AgentPeerActionCodeString(sent.code), "transaction-sent");
    BOOST_REQUIRE(sent.tx_action.has_value());
    BOOST_CHECK_EQUAL(sent.tx_action->result.code, agent::TxPeerResultCode::TX_SENT);
    BOOST_CHECK_EQUAL(sent.queued_outbound_count, 1U);
    BOOST_CHECK_EQUAL(peer.KnownTxInventoryCount(), 3U);

    const auto tx_message{peer.PopOutboundMessage()};
    BOOST_REQUIRE(tx_message.has_value());
    BOOST_CHECK_EQUAL(tx_message->m_type, NetMsgType::TX);
    const auto decoded_tx{agent::DecodeTxMessage(*tx_message)};
    BOOST_REQUIRE(decoded_tx.ok());
    BOOST_REQUIRE(decoded_tx.transaction);
    BOOST_CHECK(decoded_tx.transaction->GetHash() == transaction->GetHash());
    BOOST_CHECK(!peer.HasOutboundMessages());
}

BOOST_AUTO_TEST_CASE(agent_peer_preserves_outbound_call_order)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    agent::AgentPeer peer{chain};
    const std::vector<CInv> inventory{CInv{MSG_TX, ArithToUint256(44)}};

    BOOST_REQUIRE(peer.ProcessMessage(agent::MakeTxInvMessage(inventory)).queued_outbound_count == 1U);
    BOOST_REQUIRE(peer.StartHeaders(ArithToUint256(55)).queued_outbound_count == 1U);

    const auto messages{peer.DrainOutboundMessages()};
    BOOST_REQUIRE_EQUAL(messages.size(), 2U);
    BOOST_CHECK_EQUAL(messages[0].m_type, NetMsgType::GETDATA);
    BOOST_CHECK_EQUAL(messages[1].m_type, NetMsgType::GETHEADERS);
    BOOST_CHECK(!peer.HasOutboundMessages());
}

BOOST_AUTO_TEST_CASE(agent_peer_ignores_unhandled_messages_without_mutating_state)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    agent::AgentPeer peer{chain};

    const auto action{peer.ProcessMessage(NetMsg::Make(NetMsgType::PING, uint64_t{1}))};

    BOOST_CHECK(action.ok());
    BOOST_CHECK_EQUAL(action.code, agent::AgentPeerActionCode::IGNORED_MESSAGE);
    BOOST_CHECK_EQUAL(agent::AgentPeerActionCodeString(action.code), "ignored-message");
    BOOST_CHECK(!action.header_action.has_value());
    BOOST_CHECK(!action.tx_action.has_value());
    BOOST_CHECK_EQUAL(action.queued_outbound_count, 0U);
    BOOST_CHECK(!peer.WaitingForHeaders());
    BOOST_CHECK(!peer.HeaderPeerSynced());
    BOOST_CHECK_EQUAL(peer.PendingTxRequestCount(), 0U);
    BOOST_CHECK_EQUAL(peer.KnownTxInventoryCount(), 0U);
    BOOST_CHECK(!peer.HasOutboundMessages());
}

BOOST_AUTO_TEST_CASE(agent_peer_set_manages_peer_lifecycle)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    agent::AgentPeerSet peers{chain, uint256{}, 2, 7};

    BOOST_CHECK_EQUAL(peers.PeerCount(), 1U);
    BOOST_CHECK(peers.HasPeer(agent::DEFAULT_AGENT_PEER_ID));
    BOOST_CHECK_EQUAL(peers.MaxTxInventory(), 7U);

    const auto duplicate{peers.AddPeer(agent::DEFAULT_AGENT_PEER_ID)};
    BOOST_CHECK(!duplicate.ok());
    BOOST_CHECK_EQUAL(duplicate.code, agent::AgentPeerSetResultCode::PEER_ALREADY_EXISTS);
    BOOST_CHECK_EQUAL(agent::AgentPeerSetResultCodeString(duplicate.code), "peer-already-exists");

    const auto added{peers.AddPeer(42)};
    BOOST_CHECK(added.ok());
    BOOST_CHECK_EQUAL(added.code, agent::AgentPeerSetResultCode::PEER_ADDED);
    BOOST_CHECK_EQUAL(agent::AgentPeerSetResultCodeString(added.code), "peer-added");
    BOOST_CHECK_EQUAL(added.peer_id, 42U);
    BOOST_CHECK_EQUAL(peers.PeerCount(), 2U);
    BOOST_REQUIRE_EQUAL(peers.PeerIds().size(), 2U);
    BOOST_CHECK_EQUAL(peers.PeerIds()[0], agent::DEFAULT_AGENT_PEER_ID);
    BOOST_CHECK_EQUAL(peers.PeerIds()[1], 42U);

    const auto default_removed{peers.RemovePeer(agent::DEFAULT_AGENT_PEER_ID)};
    BOOST_CHECK(!default_removed.ok());
    BOOST_CHECK_EQUAL(default_removed.code, agent::AgentPeerSetResultCode::PEER_NOT_REMOVABLE);
    BOOST_CHECK_EQUAL(agent::AgentPeerSetResultCodeString(default_removed.code), "peer-not-removable");

    const auto removed{peers.RemovePeer(42)};
    BOOST_CHECK(removed.ok());
    BOOST_CHECK_EQUAL(removed.code, agent::AgentPeerSetResultCode::PEER_REMOVED);
    BOOST_CHECK_EQUAL(agent::AgentPeerSetResultCodeString(removed.code), "peer-removed");
    BOOST_CHECK(!peers.HasPeer(42));

    const auto missing{peers.StartHeaders(42)};
    BOOST_CHECK(!missing.ok());
    BOOST_CHECK_EQUAL(missing.code, agent::AgentPeerSetResultCode::PEER_NOT_FOUND);
    BOOST_CHECK_EQUAL(agent::AgentPeerSetResultCodeString(missing.code), "peer-not-found");
}

BOOST_AUTO_TEST_CASE(agent_peer_set_routes_headers_and_outbound_by_peer_id)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    agent::AgentPeerSet peers{chain, uint256{}, 2};
    const uint256 peer_stop_hash{ArithToUint256(42)};
    BOOST_REQUIRE(peers.AddPeer(11).ok());

    const auto started{peers.StartHeaders(11, peer_stop_hash)};

    BOOST_CHECK(started.ok());
    BOOST_CHECK_EQUAL(started.code, agent::AgentPeerSetResultCode::PEER_ACTION);
    BOOST_REQUIRE(started.peer_action.has_value());
    BOOST_CHECK_EQUAL(started.peer_action->code, agent::AgentPeerActionCode::HEADERS_STARTED);
    BOOST_CHECK_EQUAL(started.queued_outbound_count, 1U);
    BOOST_CHECK(peers.WaitingForHeaders(11));
    BOOST_CHECK(!peers.WaitingForHeaders(agent::DEFAULT_AGENT_PEER_ID));
    BOOST_CHECK(peers.HeaderStopHash(11) == peer_stop_hash);
    BOOST_CHECK_EQUAL(peers.OutboundMessageCount(11), 1U);
    BOOST_CHECK_EQUAL(peers.OutboundMessageCount(agent::DEFAULT_AGENT_PEER_ID), 0U);
    BOOST_CHECK_EQUAL(peers.OutboundMessageCount(), 1U);

    const auto request{peers.PopOutboundMessage(11)};
    BOOST_REQUIRE(request.has_value());
    BOOST_CHECK_EQUAL(request->m_type, NetMsgType::GETHEADERS);

    const CBlockHeader first{NextHeader(chain)};
    const auto processed{peers.ProcessMessage(11, agent::MakeHeadersMessage(std::span{&first, 1}))};

    BOOST_CHECK(processed.ok());
    BOOST_REQUIRE(processed.peer_action.has_value());
    BOOST_CHECK_EQUAL(processed.peer_action->code, agent::AgentPeerActionCode::HEADER_MESSAGE);
    BOOST_REQUIRE(processed.peer_action->header_action.has_value());
    BOOST_CHECK_EQUAL(processed.peer_action->header_action->driver_result.code, agent::HeaderSyncDriverResultCode::PEER_SYNCED);
    BOOST_CHECK_EQUAL(chain.Height(), 1);
    BOOST_CHECK(peers.HeaderPeerSynced(11));
    BOOST_CHECK(!peers.HasOutboundMessages());

    const auto missing{peers.ProcessMessage(99, NetMsg::Make(NetMsgType::PING, uint64_t{1}))};
    BOOST_CHECK(!missing.ok());
    BOOST_CHECK_EQUAL(missing.code, agent::AgentPeerSetResultCode::PEER_NOT_FOUND);
}

BOOST_AUTO_TEST_CASE(agent_peer_set_accepts_parallel_duplicate_header_responses)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    agent::AgentPeerSet peers{chain, uint256{}, 2};
    BOOST_REQUIRE(peers.AddPeer(11).ok());
    BOOST_REQUIRE(peers.AddPeer(12).ok());
    const CBlockHeader first{NextHeader(chain)};

    BOOST_REQUIRE(peers.StartHeaders(11).ok());
    BOOST_REQUIRE(peers.StartHeaders(12).ok());
    BOOST_REQUIRE(peers.DrainOutboundMessages(11).size() == 1U);
    BOOST_REQUIRE(peers.DrainOutboundMessages(12).size() == 1U);
    BOOST_REQUIRE(peers.ProcessMessage(11, agent::MakeHeadersMessage(std::span{&first, 1})).ok());

    const auto duplicate{peers.ProcessMessage(12, agent::MakeHeadersMessage(std::span{&first, 1}))};

    BOOST_CHECK(duplicate.ok());
    BOOST_REQUIRE(duplicate.peer_action.has_value());
    BOOST_REQUIRE(duplicate.peer_action->header_action.has_value());
    BOOST_CHECK_EQUAL(duplicate.peer_action->header_action->driver_result.code, agent::HeaderSyncDriverResultCode::PEER_SYNCED);
    BOOST_REQUIRE(duplicate.peer_action->header_action->driver_result.header_message.has_value());
    BOOST_REQUIRE(duplicate.peer_action->header_action->driver_result.header_message->sync.has_value());
    BOOST_CHECK_EQUAL(duplicate.peer_action->header_action->driver_result.header_message->sync->code, agent::HeaderSyncResultCode::DUPLICATE_HEADERS);
    BOOST_CHECK_EQUAL(duplicate.peer_action->header_action->driver_result.header_message->sync->duplicate_count, 1U);
    BOOST_CHECK_EQUAL(chain.Height(), 1);
    BOOST_CHECK(peers.HeaderPeerSynced(12));
    BOOST_CHECK(!peers.HasOutboundMessages());
}

BOOST_AUTO_TEST_CASE(agent_peer_set_fans_out_transaction_announces_to_all_peers)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    agent::AgentPeerSet peers{chain};
    BOOST_REQUIRE(peers.AddPeer(8).ok());
    const CTransactionRef transaction{TestTransaction()};

    const auto actions{peers.AnnounceTransactionToAll(*transaction)};

    BOOST_REQUIRE_EQUAL(actions.size(), 2U);
    BOOST_CHECK(actions[0].ok());
    BOOST_CHECK_EQUAL(actions[0].peer_id, agent::DEFAULT_AGENT_PEER_ID);
    BOOST_CHECK_EQUAL(actions[0].queued_outbound_count, 1U);
    BOOST_REQUIRE(actions[0].peer_action.has_value());
    BOOST_CHECK_EQUAL(actions[0].peer_action->code, agent::AgentPeerActionCode::TRANSACTION_ANNOUNCED);
    BOOST_CHECK(actions[1].ok());
    BOOST_CHECK_EQUAL(actions[1].peer_id, 8U);
    BOOST_CHECK_EQUAL(actions[1].queued_outbound_count, 1U);
    BOOST_CHECK_EQUAL(peers.KnownTxInventoryCount(agent::DEFAULT_AGENT_PEER_ID), 3U);
    BOOST_CHECK_EQUAL(peers.KnownTxInventoryCount(8), 3U);
    BOOST_CHECK_EQUAL(peers.OutboundMessageCount(), 2U);

    const auto messages{peers.DrainOutboundMessages()};
    BOOST_REQUIRE_EQUAL(messages.size(), 2U);
    BOOST_CHECK_EQUAL(messages[0].peer_id, agent::DEFAULT_AGENT_PEER_ID);
    BOOST_CHECK_EQUAL(messages[0].message.m_type, NetMsgType::INV);
    BOOST_CHECK_EQUAL(messages[1].peer_id, 8U);
    BOOST_CHECK_EQUAL(messages[1].message.m_type, NetMsgType::INV);
    BOOST_CHECK(!peers.HasOutboundMessages());
}

BOOST_AUTO_TEST_CASE(agent_client_starts_fresh_without_header_store)
{
    agent::AgentClient client{Params().GetConsensus(), Params().GenesisBlock()};
    const uint256 stop_hash{ArithToUint256(88)};

    BOOST_CHECK(client.LastLoadResult().ok());
    BOOST_CHECK_EQUAL(client.LastLoadResult().status, agent::HeaderStoreResult::FILE_NOT_FOUND);
    BOOST_CHECK_EQUAL(client.HeaderHeight(), 0);
    BOOST_CHECK(client.HeaderTip().GetBlockHash() == Params().GenesisBlock().GetHash());
    BOOST_CHECK_EQUAL(client.SaveHeaders(), agent::HeaderStoreResult::FILE_NOT_FOUND);
    BOOST_CHECK_EQUAL(agent::HeaderStoreResultString(client.LastLoadResult().status), "file-not-found");

    const auto action{client.StartHeaders(stop_hash)};

    BOOST_CHECK(action.ok());
    BOOST_CHECK_EQUAL(action.code, agent::AgentPeerActionCode::HEADERS_STARTED);
    BOOST_CHECK(client.WaitingForHeaders());
    BOOST_CHECK(client.HeaderStopHash() == stop_hash);
    BOOST_CHECK_EQUAL(client.OutboundMessageCount(), 1U);

    const auto message{client.PopOutboundMessage()};
    BOOST_REQUIRE(message.has_value());
    BOOST_CHECK_EQUAL(message->m_type, NetMsgType::GETHEADERS);
    BOOST_CHECK(!client.HasOutboundMessages());
}

BOOST_AUTO_TEST_CASE(agent_client_loads_saves_and_reloads_headers)
{
    const fs::path path{HeaderStorePath("agent_client_headers.dat")};
    auto saved_chain{ChainWithHeaders(2)};
    BOOST_REQUIRE_EQUAL(agent::SaveHeaderChain(*saved_chain, path), agent::HeaderStoreResult::OK);

    agent::AgentClient client{
        Params().GetConsensus(),
        Params().GenesisBlock(),
        agent::AgentClientOptions{.header_store_path = path, .max_headers_result = 2},
    };

    BOOST_CHECK(client.LastLoadResult().ok());
    BOOST_CHECK_EQUAL(client.LastLoadResult().status, agent::HeaderStoreResult::OK);
    BOOST_CHECK_EQUAL(client.HeaderHeight(), 2);
    BOOST_CHECK(client.HeaderTip().GetBlockHash() == saved_chain->Tip().GetBlockHash());

    const CBlockHeader next{NextHeader(client.Headers())};
    BOOST_REQUIRE(client.StartHeaders().queued_outbound_count == 1U);
    BOOST_REQUIRE(client.PopOutboundMessage().has_value());
    const auto processed{client.ProcessMessage(agent::MakeHeadersMessage(std::span{&next, 1}))};

    BOOST_CHECK(processed.ok());
    BOOST_CHECK_EQUAL(processed.code, agent::AgentPeerActionCode::HEADER_MESSAGE);
    BOOST_REQUIRE(processed.header_action.has_value());
    BOOST_CHECK_EQUAL(processed.header_action->driver_result.code, agent::HeaderSyncDriverResultCode::PEER_SYNCED);
    BOOST_CHECK_EQUAL(client.HeaderHeight(), 3);
    BOOST_CHECK(client.HeaderPeerSynced());
    BOOST_CHECK_EQUAL(client.SaveHeaders(), agent::HeaderStoreResult::OK);

    agent::AgentClient reloaded{
        Params().GetConsensus(),
        Params().GenesisBlock(),
        agent::AgentClientOptions{.header_store_path = path},
    };
    BOOST_CHECK_EQUAL(reloaded.LastLoadResult().status, agent::HeaderStoreResult::OK);
    BOOST_CHECK_EQUAL(reloaded.HeaderHeight(), 3);
    BOOST_CHECK(reloaded.HeaderTip().GetBlockHash() == next.GetHash());
}

BOOST_AUTO_TEST_CASE(agent_client_reports_store_load_failure_and_keeps_genesis)
{
    const fs::path path{HeaderStorePath("agent_client_bad_headers.dat")};
    const std::vector<CBlockHeader> headers{Params().GenesisBlock()};
    WriteRawHeaderStore(path, 0, TEST_HEADER_STORE_VERSION, Params().GenesisBlock().GetHash(), headers);

    agent::AgentClient client{
        Params().GetConsensus(),
        Params().GenesisBlock(),
        agent::AgentClientOptions{.header_store_path = path},
    };

    BOOST_CHECK(!client.LastLoadResult().ok());
    BOOST_CHECK_EQUAL(client.LastLoadResult().status, agent::HeaderStoreResult::BAD_MAGIC);
    BOOST_CHECK_EQUAL(agent::HeaderStoreResultString(client.LastLoadResult().status), "bad-magic");
    BOOST_CHECK_EQUAL(client.HeaderHeight(), 0);
    BOOST_CHECK(client.HeaderTip().GetBlockHash() == Params().GenesisBlock().GetHash());
    BOOST_CHECK(!client.HasOutboundMessages());
}

BOOST_AUTO_TEST_CASE(agent_client_delegates_transaction_relay_to_peer_facade)
{
    agent::AgentClient client{
        Params().GetConsensus(),
        Params().GenesisBlock(),
        agent::AgentClientOptions{.max_tx_inventory = 7},
    };
    const CTransactionRef transaction{TestTransaction()};

    const auto sent{client.SendTransaction(*transaction)};

    BOOST_CHECK(sent.ok());
    BOOST_CHECK_EQUAL(sent.code, agent::AgentPeerActionCode::TRANSACTION_SENT);
    BOOST_REQUIRE(sent.tx_action.has_value());
    BOOST_CHECK_EQUAL(sent.tx_action->result.code, agent::TxPeerResultCode::TX_SENT);
    BOOST_CHECK_EQUAL(sent.queued_outbound_count, 1U);
    BOOST_CHECK_EQUAL(client.KnownTxInventoryCount(), 3U);
    BOOST_CHECK_EQUAL(client.PendingTxRequestCount(), 0U);
    BOOST_CHECK_EQUAL(client.OutboundMessageCount(), 1U);

    const auto tx_message{client.PopOutboundMessage()};
    BOOST_REQUIRE(tx_message.has_value());
    BOOST_CHECK_EQUAL(tx_message->m_type, NetMsgType::TX);
    const auto decoded_tx{agent::DecodeTxMessage(*tx_message)};
    BOOST_REQUIRE(decoded_tx.ok());
    BOOST_REQUIRE(decoded_tx.transaction);
    BOOST_CHECK(decoded_tx.transaction->GetWitnessHash() == transaction->GetWitnessHash());
    BOOST_CHECK(!client.HasOutboundMessages());
}

BOOST_AUTO_TEST_CASE(agent_client_exposes_transport_supplied_peer_ids)
{
    agent::AgentClient client{
        Params().GetConsensus(),
        Params().GenesisBlock(),
        agent::AgentClientOptions{.max_headers_result = 2, .max_tx_inventory = 5},
    };
    const std::vector<CInv> inventory{CInv{MSG_TX, ArithToUint256(91)}};

    BOOST_CHECK_EQUAL(client.PeerCount(), 1U);
    BOOST_CHECK(client.HasPeer(agent::DEFAULT_AGENT_PEER_ID));
    BOOST_REQUIRE(client.AddPeer(3).ok());
    BOOST_CHECK_EQUAL(client.PeerCount(), 2U);

    const auto action{client.ProcessMessage(3, agent::MakeTxInvMessage(inventory))};

    BOOST_CHECK(action.ok());
    BOOST_CHECK_EQUAL(action.code, agent::AgentPeerSetResultCode::PEER_ACTION);
    BOOST_REQUIRE(action.peer_action.has_value());
    BOOST_CHECK_EQUAL(action.peer_action->code, agent::AgentPeerActionCode::TX_MESSAGE);
    BOOST_REQUIRE(action.peer_action->tx_action.has_value());
    BOOST_CHECK_EQUAL(action.peer_action->tx_action->result.code, agent::TxPeerResultCode::GETDATA_SENT);
    BOOST_CHECK_EQUAL(action.queued_outbound_count, 1U);
    BOOST_CHECK_EQUAL(client.PendingTxRequestCount(3), 1U);
    BOOST_CHECK_EQUAL(client.PendingTxRequestCount(agent::DEFAULT_AGENT_PEER_ID), 0U);
    BOOST_CHECK_EQUAL(client.OutboundMessageCount(3), 1U);
    BOOST_CHECK_EQUAL(client.OutboundMessageCount(), 0U);
    BOOST_CHECK(client.HasOutboundMessages(3));

    const auto outbound{client.PopOutboundMessage(3)};
    BOOST_REQUIRE(outbound.has_value());
    BOOST_CHECK_EQUAL(outbound->m_type, NetMsgType::GETDATA);
    BOOST_CHECK(!client.HasOutboundMessages(3));
}

BOOST_AUTO_TEST_CASE(agent_allotment_policy_decodes_desktop_policy_request)
{
    const std::string policy_request{strprintf(
        R"({"type":"quicksilver.agent_allotment_policy_request","version":1,"chain":"%s","genesis_hash":"%s","id":"agent-1","label":"test-agent","funding_address":"agent-address","funding_limit_cinnabar":"100000000","funding_available_cinnabar":"75000000","daily_limit_cinnabar":"25000000","risk_accepted_time":"123","request_created_time":"456","policy_status":"pending_integration","backend_created":false})",
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash().ToString())};

    auto artifact{agent::DecodeAllotmentPolicyRequest(policy_request, Params().GetChainTypeString(), Params().GenesisBlock().GetHash().ToString())};

    BOOST_REQUIRE_MESSAGE(artifact, util::ErrorString(artifact).original);
    BOOST_CHECK_EQUAL(artifact->id, "agent-1");
    BOOST_CHECK_EQUAL(artifact->label, "test-agent");
    BOOST_CHECK_EQUAL(artifact->chain, Params().GetChainTypeString());
    BOOST_CHECK_EQUAL(artifact->genesis_hash, Params().GenesisBlock().GetHash().ToString());
    BOOST_CHECK_EQUAL(artifact->funding_address, "agent-address");
    BOOST_CHECK_EQUAL(artifact->policy.funding_limit, COIN);
    BOOST_CHECK_EQUAL(artifact->funding_available, 75 * COIN / 100);
    BOOST_CHECK_EQUAL(artifact->policy.daily_limit, COIN / 4);
    BOOST_CHECK_EQUAL(artifact->risk_accepted_time, 123);
    BOOST_CHECK_EQUAL(artifact->request_created_time, 456);
    BOOST_CHECK_EQUAL(artifact->policy_status, "pending_integration");
    BOOST_CHECK(!artifact->backend_created);
}

BOOST_AUTO_TEST_CASE(agent_allotment_policy_decodes_desktop_key_bundle)
{
    const std::string policy_request{strprintf(
        R"({"type":"quicksilver.agent_allotment_policy_request","version":1,"chain":"%s","genesis_hash":"%s","id":"agent-1","label":"test-agent","funding_address":"agent-address","funding_limit_cinnabar":"100000000","funding_available_cinnabar":"75000000","daily_limit_cinnabar":"25000000","risk_accepted_time":"123","request_created_time":"456","policy_status":"pending_integration","backend_created":false})",
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash().ToString())};
    const std::string bundle{strprintf(
        R"({"type":"quicksilver.agent_allotment_key_bundle","version":1,"policy_request":%s,"funding_address":"agent-address","funding_secret_wif":"agent-secret","policy_enforcement":"pending_integration"})",
        policy_request)};

    auto artifact{agent::DecodeAllotmentPolicyBundle(bundle, Params().GetChainTypeString(), Params().GenesisBlock().GetHash().ToString())};

    BOOST_REQUIRE_MESSAGE(artifact, util::ErrorString(artifact).original);
    BOOST_CHECK_EQUAL(artifact->policy_request.id, "agent-1");
    BOOST_CHECK_EQUAL(artifact->policy_request.label, "test-agent");
    BOOST_CHECK_EQUAL(artifact->policy_request.funding_address, "agent-address");
    BOOST_CHECK_EQUAL(artifact->policy_request.policy.funding_limit, COIN);
    BOOST_CHECK_EQUAL(artifact->policy_request.funding_available, 75 * COIN / 100);
    BOOST_CHECK_EQUAL(artifact->policy_request.policy.daily_limit, COIN / 4);
    BOOST_CHECK_EQUAL(artifact->funding_address, "agent-address");
    BOOST_CHECK_EQUAL(artifact->funding_secret, "agent-secret");
    BOOST_CHECK_EQUAL(artifact->policy_enforcement, "pending_integration");
    BOOST_CHECK(artifact->funding_outputs.empty());
    BOOST_CHECK(!artifact->policy_request_json.empty());
}

BOOST_AUTO_TEST_CASE(agent_allotment_policy_decodes_bundle_funding_outputs)
{
    const CKey funding_key{GenerateRandomKey()};
    const std::vector<agent::AllotmentFundingOutputArtifact> funding_outputs{
        {
            .txid = Txid::FromUint256(ArithToUint256(21)).ToString(),
            .vout = 0,
            .amount = COIN / 4,
        },
        {
            .txid = Txid::FromUint256(ArithToUint256(22)).ToString(),
            .vout = 3,
            .amount = COIN / 2,
        },
    };

    auto artifact{agent::DecodeAllotmentPolicyBundle(
        AgentAllotmentBundleJson(funding_key, /*funding_available=*/3 * COIN / 4, /*daily_limit=*/COIN, funding_outputs),
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash().ToString())};

    BOOST_REQUIRE_MESSAGE(artifact, util::ErrorString(artifact).original);
    BOOST_REQUIRE_EQUAL(artifact->funding_outputs.size(), 2U);
    BOOST_CHECK_EQUAL(artifact->funding_outputs[0].txid, funding_outputs[0].txid);
    BOOST_CHECK_EQUAL(artifact->funding_outputs[0].vout, 0U);
    BOOST_CHECK_EQUAL(artifact->funding_outputs[0].amount, COIN / 4);
    BOOST_CHECK_EQUAL(artifact->funding_outputs[1].txid, funding_outputs[1].txid);
    BOOST_CHECK_EQUAL(artifact->funding_outputs[1].vout, 3U);
    BOOST_CHECK_EQUAL(artifact->funding_outputs[1].amount, COIN / 2);

    std::string bad_bundle{AgentAllotmentBundleJson(funding_key, /*funding_available=*/COIN, /*daily_limit=*/COIN, funding_outputs)};
    const size_t txid_pos{bad_bundle.find(funding_outputs[0].txid)};
    BOOST_REQUIRE_NE(txid_pos, std::string::npos);
    bad_bundle.replace(txid_pos, funding_outputs[0].txid.size(), "not-a-txid");
    auto bad_artifact{agent::DecodeAllotmentPolicyBundle(bad_bundle, Params().GetChainTypeString(), Params().GenesisBlock().GetHash().ToString())};
    BOOST_CHECK(!bad_artifact);
    BOOST_CHECK_EQUAL(util::ErrorString(bad_artifact).original, "Agent allotment policy bundle funding output txid must be a transaction id hex string.");
}

BOOST_AUTO_TEST_CASE(agent_allotment_policy_decodes_payment_receipt)
{
    const CKey funding_key{GenerateRandomKey()};
    const std::string funding_address{EncodeDestination(WitnessV0KeyHash(funding_key.GetPubKey()))};
    const agent::AllotmentFundingOutputArtifact funding_output{
        .txid = Txid::FromUint256(ArithToUint256(51)).ToString(),
        .vout = 4,
        .amount = COIN / 6,
    };

    auto receipt{agent::DecodeAllotmentPaymentReceipt(
        AgentAllotmentPaymentReceiptJson(funding_address, funding_output),
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash().ToString(),
        funding_address)};

    BOOST_REQUIRE_MESSAGE(receipt, util::ErrorString(receipt).original);
    BOOST_CHECK_EQUAL(receipt->chain, Params().GetChainTypeString());
    BOOST_CHECK_EQUAL(receipt->genesis_hash, Params().GenesisBlock().GetHash().ToString());
    BOOST_CHECK_EQUAL(receipt->funding_address, funding_address);
    BOOST_CHECK_EQUAL(receipt->funding_output.txid, funding_output.txid);
    BOOST_CHECK_EQUAL(receipt->funding_output.vout, 4U);
    BOOST_CHECK_EQUAL(receipt->funding_output.amount, COIN / 6);
    BOOST_CHECK_EQUAL(receipt->received_time, 789);
    BOOST_CHECK(receipt->payment_id.empty());
    BOOST_CHECK(receipt->label.empty());
    BOOST_CHECK(receipt->memo.empty());
    BOOST_CHECK(receipt->payer.empty());

    auto metadata_receipt{agent::DecodeAllotmentPaymentReceipt(
        AgentAllotmentPaymentReceiptJson(
            funding_address,
            funding_output,
            790,
            R"(,"payment_id":"checkout-42","label":"Desktop invoice","memo":"receipt for local agent funding","payer":"local desk")"),
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash().ToString(),
        funding_address)};
    BOOST_REQUIRE_MESSAGE(metadata_receipt, util::ErrorString(metadata_receipt).original);
    BOOST_CHECK_EQUAL(metadata_receipt->received_time, 790);
    BOOST_CHECK_EQUAL(metadata_receipt->payment_id, "checkout-42");
    BOOST_CHECK_EQUAL(metadata_receipt->label, "Desktop invoice");
    BOOST_CHECK_EQUAL(metadata_receipt->memo, "receipt for local agent funding");
    BOOST_CHECK_EQUAL(metadata_receipt->payer, "local desk");

    auto wrong_address{agent::DecodeAllotmentPaymentReceipt(
        AgentAllotmentPaymentReceiptJson(funding_address, funding_output),
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash().ToString(),
        "wrong-address")};
    BOOST_CHECK(!wrong_address);
    BOOST_CHECK_EQUAL(util::ErrorString(wrong_address).original, "Agent allotment payment receipt funding address does not match the bundle.");

    std::string bad_receipt{AgentAllotmentPaymentReceiptJson(funding_address, funding_output)};
    const size_t txid_pos{bad_receipt.find(funding_output.txid)};
    BOOST_REQUIRE_NE(txid_pos, std::string::npos);
    bad_receipt.replace(txid_pos, funding_output.txid.size(), "not-a-txid");
    auto bad_txid{agent::DecodeAllotmentPaymentReceipt(
        bad_receipt,
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash().ToString(),
        funding_address)};
    BOOST_CHECK(!bad_txid);
    BOOST_CHECK_EQUAL(util::ErrorString(bad_txid).original, "Agent allotment payment receipt txid must be a transaction id hex string.");
}

BOOST_AUTO_TEST_CASE(agent_allotment_payment_receipts_round_trip_store)
{
    const CKey funding_key{GenerateRandomKey()};
    const std::string funding_address{EncodeDestination(WitnessV0KeyHash(funding_key.GetPubKey()))};
    const std::vector<agent::AllotmentPaymentReceiptArtifact> receipts{
        agent::AllotmentPaymentReceiptArtifact{
            .chain = Params().GetChainTypeString(),
            .genesis_hash = Params().GenesisBlock().GetHash().ToString(),
            .funding_address = funding_address,
            .funding_output = agent::AllotmentFundingOutputArtifact{
                .txid = Txid::FromUint256(ArithToUint256(61)).ToString(),
                .vout = 1,
                .amount = COIN / 7,
            },
            .received_time = 1234,
            .payment_id = "payment-61",
            .label = "Receipt 61",
            .memo = "first stored payment",
            .payer = "desktop",
        },
        agent::AllotmentPaymentReceiptArtifact{
            .chain = Params().GetChainTypeString(),
            .genesis_hash = Params().GenesisBlock().GetHash().ToString(),
            .funding_address = funding_address,
            .funding_output = agent::AllotmentFundingOutputArtifact{
                .txid = Txid::FromUint256(ArithToUint256(62)).ToString(),
                .vout = 2,
                .amount = COIN / 8,
            },
            .received_time = 1235,
            .payment_id = "payment-62",
            .label = "Receipt 62",
            .memo = "second stored payment",
            .payer = "agent",
        },
    };
    const std::vector<agent::AllotmentReceiptActivity> activities{
        {
            .type = agent::AllotmentReceiptActivityType::IMPORTED,
            .funding_address = funding_address,
            .funding_output = receipts[0].funding_output,
            .event_time = 1234,
            .related_txid = "",
            .payment_id = receipts[0].payment_id,
            .label = receipts[0].label,
            .memo = receipts[0].memo,
            .payer = receipts[0].payer,
        },
        {
            .type = agent::AllotmentReceiptActivityType::SPENT,
            .funding_address = funding_address,
            .funding_output = receipts[1].funding_output,
            .event_time = 1240,
            .related_txid = Txid::FromUint256(ArithToUint256(70)).ToString(),
            .payment_id = receipts[1].payment_id,
            .label = receipts[1].label,
            .memo = receipts[1].memo,
            .payer = receipts[1].payer,
        },
    };
    const fs::path path{HeaderStorePath("allotment-receipts.dat")};

    BOOST_CHECK_EQUAL(agent::AllotmentStoreResultString(agent::SaveAllotmentReceiptStore({.receipts = receipts, .activities = activities}, path, Params().GenesisBlock().GetHash())), "ok");
    auto loaded{agent::LoadAllotmentPaymentReceipts(path, Params().GenesisBlock().GetHash())};

    BOOST_REQUIRE(loaded.ok());
    BOOST_REQUIRE_EQUAL(loaded.receipts.size(), receipts.size());
    BOOST_CHECK_EQUAL(loaded.receipts[0].funding_address, funding_address);
    BOOST_CHECK_EQUAL(loaded.receipts[0].funding_output.txid, receipts[0].funding_output.txid);
    BOOST_CHECK_EQUAL(loaded.receipts[0].funding_output.vout, 1U);
    BOOST_CHECK_EQUAL(loaded.receipts[0].funding_output.amount, COIN / 7);
    BOOST_CHECK_EQUAL(loaded.receipts[0].received_time, 1234);
    BOOST_CHECK_EQUAL(loaded.receipts[0].payment_id, receipts[0].payment_id);
    BOOST_CHECK_EQUAL(loaded.receipts[0].label, receipts[0].label);
    BOOST_CHECK_EQUAL(loaded.receipts[0].memo, receipts[0].memo);
    BOOST_CHECK_EQUAL(loaded.receipts[0].payer, receipts[0].payer);
    BOOST_CHECK_EQUAL(loaded.receipts[1].funding_output.txid, receipts[1].funding_output.txid);
    BOOST_CHECK_EQUAL(loaded.receipts[1].funding_output.vout, 2U);
    BOOST_CHECK_EQUAL(loaded.receipts[1].funding_output.amount, COIN / 8);
    BOOST_CHECK_EQUAL(loaded.receipts[1].received_time, 1235);
    BOOST_CHECK_EQUAL(loaded.receipts[1].payment_id, receipts[1].payment_id);
    BOOST_REQUIRE_EQUAL(loaded.activities.size(), activities.size());
    BOOST_CHECK_EQUAL(agent::AllotmentReceiptActivityTypeString(loaded.activities[0].type), "imported");
    BOOST_CHECK_EQUAL(loaded.activities[0].funding_address, funding_address);
    BOOST_CHECK_EQUAL(loaded.activities[0].funding_output.txid, receipts[0].funding_output.txid);
    BOOST_CHECK_EQUAL(loaded.activities[0].funding_output.vout, 1U);
    BOOST_CHECK_EQUAL(loaded.activities[0].funding_output.amount, COIN / 7);
    BOOST_CHECK_EQUAL(loaded.activities[0].event_time, 1234);
    BOOST_CHECK_EQUAL(loaded.activities[0].payment_id, activities[0].payment_id);
    BOOST_CHECK_EQUAL(loaded.activities[0].label, activities[0].label);
    BOOST_CHECK_EQUAL(loaded.activities[0].memo, activities[0].memo);
    BOOST_CHECK_EQUAL(loaded.activities[0].payer, activities[0].payer);
    BOOST_CHECK_EQUAL(agent::AllotmentReceiptActivityTypeString(loaded.activities[1].type), "spent");
    BOOST_CHECK_EQUAL(loaded.activities[1].funding_output.txid, receipts[1].funding_output.txid);
    BOOST_CHECK_EQUAL(loaded.activities[1].related_txid, activities[1].related_txid);
    BOOST_CHECK_EQUAL(loaded.activities[1].payment_id, activities[1].payment_id);

    auto wrong_genesis{agent::LoadAllotmentPaymentReceipts(path, ArithToUint256(99))};
    BOOST_CHECK(!wrong_genesis.ok());
    BOOST_CHECK_EQUAL(agent::AllotmentStoreResultString(wrong_genesis.status), "bad-genesis");
}

BOOST_AUTO_TEST_CASE(agent_allotment_payment_receipt_directory_scan_is_durable_and_idempotent)
{
    const CKey funding_key{GenerateRandomKey()};
    const std::string funding_address{EncodeDestination(WitnessV0KeyHash(funding_key.GetPubKey()))};
    const agent::AllotmentFundingOutputArtifact output{
        .txid = Txid::FromUint256(ArithToUint256(81)).ToString(),
        .vout = 3,
        .amount = COIN / 4,
    };
    const fs::path directory{HeaderStorePath("payment-receipts.d")};
    const fs::path store_path{HeaderStorePath("scanned-allotment-receipts.dat")};
    fs::create_directories(directory);
    BOOST_REQUIRE(WriteBinaryFile(directory / "receipt.json", AgentAllotmentPaymentReceiptJson(funding_address, output) + "\n"));

    auto first_scan{agent::ScanAllotmentPaymentReceiptDirectory(
        directory,
        store_path,
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash())};
    BOOST_REQUIRE(first_scan);
    BOOST_CHECK_EQUAL(first_scan->scanned_files, 1U);
    BOOST_CHECK_EQUAL(first_scan->imported_receipts, 1U);
    BOOST_REQUIRE_EQUAL(first_scan->store.receipts.size(), 1U);
    BOOST_REQUIRE_EQUAL(first_scan->store.activities.size(), 1U);
    BOOST_CHECK_EQUAL(first_scan->store.receipts[0].funding_output.txid, output.txid);
    BOOST_CHECK_EQUAL(first_scan->store.activities[0].type, agent::AllotmentReceiptActivityType::IMPORTED);

    auto second_scan{agent::ScanAllotmentPaymentReceiptDirectory(
        directory,
        store_path,
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash())};
    BOOST_REQUIRE(second_scan);
    BOOST_CHECK_EQUAL(second_scan->imported_receipts, 0U);
    BOOST_CHECK_EQUAL(second_scan->store.receipts.size(), 1U);
    BOOST_CHECK_EQUAL(second_scan->store.activities.size(), 1U);

    agent::AllotmentReceiptStoreData spent_store{std::move(second_scan->store)};
    spent_store.receipts.clear();
    spent_store.activities.push_back(agent::AllotmentReceiptActivity{
        .type = agent::AllotmentReceiptActivityType::SPENT,
        .funding_address = funding_address,
        .funding_output = output,
        .event_time = 1300,
        .related_txid = Txid::FromUint256(ArithToUint256(82)).ToString(),
        .payment_id = {},
        .label = {},
        .memo = {},
        .payer = {},
    });
    BOOST_REQUIRE(agent::SaveAllotmentReceiptStore(spent_store, store_path, Params().GenesisBlock().GetHash()) == agent::AllotmentStoreResult::OK);
    auto spent_rescan{agent::ScanAllotmentPaymentReceiptDirectory(
        directory,
        store_path,
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash())};
    BOOST_REQUIRE(spent_rescan);
    BOOST_CHECK_EQUAL(spent_rescan->imported_receipts, 0U);
    BOOST_CHECK(spent_rescan->store.receipts.empty());
    BOOST_CHECK_EQUAL(spent_rescan->store.activities.size(), 2U);

    agent::AllotmentFundingOutputArtifact conflicting_output{output};
    ++conflicting_output.amount;
    BOOST_REQUIRE(WriteBinaryFile(directory / "conflict.json", AgentAllotmentPaymentReceiptJson(funding_address, conflicting_output) + "\n"));
    auto conflict{agent::ScanAllotmentPaymentReceiptDirectory(
        directory,
        store_path,
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash())};
    BOOST_CHECK(!conflict);
    BOOST_CHECK(util::ErrorString(conflict).original.find("duplicates a spent funding output") != std::string::npos);

    auto persisted{agent::LoadAllotmentReceiptStore(store_path, Params().GetChainTypeString(), Params().GenesisBlock().GetHash())};
    BOOST_REQUIRE(persisted);
    BOOST_CHECK(persisted->receipts.empty());
    BOOST_CHECK_EQUAL(persisted->activities.size(), 2U);
}

BOOST_AUTO_TEST_CASE(agent_allotment_policy_rejects_wrong_policy_request_metadata)
{
    const std::string policy_request{strprintf(
        R"({"type":"quicksilver.agent_allotment_policy_request","version":1,"chain":"%s","genesis_hash":"%s","id":"agent-1","label":"test-agent","funding_address":"agent-address","funding_limit_cinnabar":"100000000","funding_available_cinnabar":"75000000","daily_limit_cinnabar":"25000000","risk_accepted_time":"123","request_created_time":"456","policy_status":"pending_integration","backend_created":false})",
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash().ToString())};

    BOOST_CHECK(!agent::DecodeAllotmentPolicyRequest("[]", Params().GetChainTypeString(), Params().GenesisBlock().GetHash().ToString()));
    auto wrong_chain{agent::DecodeAllotmentPolicyRequest(policy_request, "wrong-chain", Params().GenesisBlock().GetHash().ToString())};
    BOOST_CHECK(!wrong_chain);
    BOOST_CHECK_EQUAL(util::ErrorString(wrong_chain).original, "Agent allotment policy request is for a different chain.");

    std::string bad_status{policy_request};
    const std::string pending_status{R"("policy_status":"pending_integration")"};
    const size_t status_pos{bad_status.find(pending_status)};
    BOOST_REQUIRE_NE(status_pos, std::string::npos);
    bad_status.replace(status_pos, pending_status.size(), R"("policy_status":"unknown")");
    BOOST_REQUIRE_NE(bad_status, policy_request);
    auto unknown_status{agent::DecodeAllotmentPolicyRequest(bad_status, Params().GetChainTypeString(), Params().GenesisBlock().GetHash().ToString())};
    BOOST_CHECK(!unknown_status);
    BOOST_CHECK_EQUAL(util::ErrorString(unknown_status).original, "Agent allotment policy request has an unknown policy_status.");

    const std::string bundle{strprintf(
        R"({"type":"quicksilver.agent_allotment_key_bundle","version":1,"policy_request":%s,"funding_address":"wrong-address","funding_secret_wif":"agent-secret","policy_enforcement":"pending_integration"})",
        policy_request)};
    auto wrong_bundle_address{agent::DecodeAllotmentPolicyBundle(bundle, Params().GetChainTypeString(), Params().GenesisBlock().GetHash().ToString())};
    BOOST_CHECK(!wrong_bundle_address);
    BOOST_CHECK_EQUAL(util::ErrorString(wrong_bundle_address).original, "Agent allotment policy bundle funding address does not match the policy request.");
}

BOOST_AUTO_TEST_CASE(agent_allotment_policy_checks_daily_guardrail)
{
    const agent::AllotmentPolicy policy{.funding_limit = COIN, .daily_limit = COIN / 2};

    auto allowed{agent::CheckAllotmentSpend(policy, {.funding_available = COIN, .spent_today = 0}, COIN / 4)};
    BOOST_CHECK(allowed.allowed());
    BOOST_CHECK_EQUAL(allowed.code, agent::AllotmentPolicyResultCode::ALLOWED);
    BOOST_CHECK_EQUAL(allowed.daily_remaining, COIN / 4);

    allowed = agent::CheckAllotmentSpend(policy, {.funding_available = COIN, .spent_today = COIN / 4}, COIN / 4);
    BOOST_CHECK(allowed.allowed());
    BOOST_CHECK_EQUAL(allowed.daily_remaining, 0);

    const auto exceeded{agent::CheckAllotmentSpend(policy, {.funding_available = COIN, .spent_today = COIN / 4}, COIN / 2)};
    BOOST_CHECK(!exceeded.allowed());
    BOOST_CHECK_EQUAL(exceeded.code, agent::AllotmentPolicyResultCode::DAILY_LIMIT_EXCEEDED);
    BOOST_CHECK_EQUAL(exceeded.daily_remaining, COIN / 4);
    BOOST_CHECK_EQUAL(agent::AllotmentPolicyResultCodeString(exceeded.code), "daily-limit-exceeded");
}

BOOST_AUTO_TEST_CASE(agent_allotment_policy_allows_unlimited_daily_guardrail)
{
    const agent::AllotmentPolicy policy{.funding_limit = COIN, .daily_limit = 0};

    const auto allowed{agent::CheckAllotmentSpend(policy, {.funding_available = COIN, .spent_today = COIN / 2}, COIN)};

    BOOST_CHECK(allowed.allowed());
    BOOST_CHECK_EQUAL(allowed.code, agent::AllotmentPolicyResultCode::ALLOWED);
    BOOST_CHECK_EQUAL(allowed.daily_remaining, 0);
}

BOOST_AUTO_TEST_CASE(agent_allotment_policy_rejects_invalid_or_unfunded_spends)
{
    const agent::AllotmentPolicy policy{.funding_limit = COIN, .daily_limit = COIN / 2};

    BOOST_CHECK_EQUAL(agent::CheckAllotmentSpend({.funding_limit = 0, .daily_limit = COIN / 2}, {.funding_available = COIN, .spent_today = 0}, COIN / 4).code,
                      agent::AllotmentPolicyResultCode::INVALID_POLICY);
    BOOST_CHECK_EQUAL(agent::CheckAllotmentSpend(policy, {.funding_available = COIN, .spent_today = 0}, 0).code,
                      agent::AllotmentPolicyResultCode::INVALID_AMOUNT);
    BOOST_CHECK_EQUAL(agent::CheckAllotmentSpend(policy, {.funding_available = COIN / 4, .spent_today = 0}, COIN / 2).code,
                      agent::AllotmentPolicyResultCode::INSUFFICIENT_FUNDS);
    BOOST_CHECK_EQUAL(agent::AllotmentPolicyResultCodeString(agent::AllotmentPolicyResultCode::INSUFFICIENT_FUNDS), "insufficient-funds");
}

BOOST_AUTO_TEST_CASE(agent_allotment_imports_bundle_and_signs_known_output_spend)
{
    const CKey funding_key{GenerateRandomKey()};
    auto context{agent::ImportAllotmentBundle(
        AgentAllotmentBundleJson(funding_key),
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash().ToString())};
    BOOST_REQUIRE_MESSAGE(context, util::ErrorString(context).original);

    const CKey destination_key{GenerateRandomKey()};
    const CAmount prevout_value{COIN};
    const CAmount spend_amount{COIN / 4};
    const agent::AllotmentSpendRequest request{
        .prevout = COutPoint{Txid::FromUint256(ArithToUint256(11)), 0},
        .prevout_value = prevout_value,
        .destination = PKHash(destination_key.GetPubKey()),
        .spend_amount = spend_amount,
        .spent_today = COIN / 8,
        .prove = false,
    };

    auto signed_spend{agent::CreateSignedAllotmentSpend(*context, request, Params().GetConsensus())};
    BOOST_REQUIRE_MESSAGE(signed_spend, util::ErrorString(signed_spend).original);

    const CTransaction& tx{signed_spend->transaction};
    BOOST_REQUIRE_EQUAL(tx.vin.size(), 1U);
    BOOST_REQUIRE_EQUAL(tx.vout.size(), 2U);
    BOOST_CHECK(tx.vin[0].prevout == request.prevout);
    BOOST_CHECK_EQUAL(tx.vout[0].nValue, spend_amount);
    BOOST_CHECK(tx.vout[0].scriptPubKey == GetScriptForDestination(request.destination));
    BOOST_CHECK_EQUAL(tx.vout[1].nValue, prevout_value - spend_amount);
    BOOST_CHECK(tx.vout[1].scriptPubKey == context->funding_script);
    BOOST_CHECK_EQUAL(signed_spend->change_amount, prevout_value - spend_amount);
    BOOST_CHECK_EQUAL(signed_spend->policy_check.code, agent::AllotmentPolicyResultCode::ALLOWED);

    ScriptError script_error{SCRIPT_ERR_OK};
    BOOST_CHECK(VerifyScript(tx.vin[0].scriptSig,
                             context->funding_script,
                             &tx.vin[0].scriptWitness,
                             STANDARD_SCRIPT_VERIFY_FLAGS,
                             TransactionSignatureChecker(&tx, 0, prevout_value, MissingDataBehavior::FAIL),
                             &script_error));
    BOOST_CHECK_EQUAL(script_error, SCRIPT_ERR_OK);
}

// --- F-153: CTRL-C MUST REACH THE GRIND -----------------------------------
// The plumbing above is only useful if something in the shipped binary actually
// trips it. quicksilver-agent installed no signal handler at all, so SIGINT took
// the default action and killed the agent outright -- leaving the solver, which
// is in its own process group precisely so a stray signal cannot reach it,
// running with the card pinned.
BOOST_AUTO_TEST_CASE(agent_interrupt_handler_cancels_the_grind_on_sigint)
{
    const agent::InterruptHandler interrupt;
    const cuckatoo::SolverCancelCallback cancel{interrupt.Cancel()};
    BOOST_REQUIRE_MESSAGE(!cancel(), "the flag must start clear");
    BOOST_REQUIRE(!interrupt.Interrupted());

    std::raise(SIGINT);

    BOOST_CHECK_MESSAGE(interrupt.Interrupted(),
                        "SIGINT was caught but not recorded; the grind would run on");
    BOOST_CHECK_MESSAGE(cancel(),
                        "the cancel callback handed to the solver must report the interrupt, "
                        "otherwise Ctrl-C orphans qsgpusolve with the card pinned");
}

// --- F-153: THE AGENT GRIND MUST BE STOPPABLE ------------------------------
// ProveAgentSpend passed /*cancel=*/{} to the solver, and its retry loop is
// UNBOUNDED: every iteration starts a fresh 2^24-attempt budget and it only
// leaves when a cycle beats the target. So there was no way to stop a signbundle
// grind at all. Worse than it sounds on POSIX -- the agent installs no signal
// handler, and gpu_solver puts the solver in its own process group, so Ctrl-C
// kills the agent and ORPHANS qsgpusolve with the card pinned.
BOOST_AUTO_TEST_CASE(agent_allotment_proof_stops_when_cancelled)
{
    Consensus::Params consensus{Params().GetConsensus()};
    consensus.nTxWorkCouplingK = 1;
    agent::HeaderChain chain{consensus, Params().GenesisBlock()};
    const CBlockHeader anchor_header{
        NextHeader(chain, /*time=*/0, /*merkle_root=*/uint256{},
                   /*congestion=*/static_cast<uint32_t>(8 * CONGESTION_ONE))};
    BOOST_REQUIRE(chain.AcceptHeader(anchor_header).accepted());
    const CBlockIndex& anchor{chain.Tip()};

    const CKey funding_key{GenerateRandomKey()};
    auto context{agent::ImportAllotmentBundle(
        AgentAllotmentBundleJson(funding_key),
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash().ToString())};
    BOOST_REQUIRE_MESSAGE(context, util::ErrorString(context).original);

    const CKey destination_key{GenerateRandomKey()};
    std::atomic<int> polls{0};
    auto signed_spend{agent::CreateSignedAllotmentSpend(
        *context,
        agent::AllotmentSpendRequest{
            .prevout = COutPoint{Txid::FromUint256(ArithToUint256(73)), 0},
            .prevout_value = COIN,
            .destination = PKHash(destination_key.GetPubKey()),
            .spend_amount = COIN / 4,
            .spent_today = 0,
            .prove = true,
            .anchor = &anchor,
            .cancel = [&] { ++polls; return true; },
        },
        consensus)};

    BOOST_REQUIRE_MESSAGE(!signed_spend,
                          "a cancelled grind must not return a signed spend; the callback was "
                          "ignored and the proof ran to completion");
    BOOST_CHECK_MESSAGE(polls.load() > 0, "the cancel callback was never polled");
    const std::string err{util::ErrorString(signed_spend).original};
    BOOST_CHECK_MESSAGE(err.find("cancel") != std::string::npos,
                        "a cancelled grind must say so rather than reading as a failure to find "
                        "a proof; got: " << err);
}

BOOST_AUTO_TEST_CASE(agent_allotment_proof_matches_anchor_derived_target)
{
    Consensus::Params consensus{Params().GetConsensus()};
    consensus.nTxWorkCouplingK = 1;
    agent::HeaderChain chain{consensus, Params().GenesisBlock()};
    // The elevated multiplier arrives IN the header, the way an agent actually gets it —
    // not written onto the index afterwards. That is the whole point of #5c-1 Phase 2.
    const CBlockHeader anchor_header{
        NextHeader(chain, /*time=*/0, /*merkle_root=*/uint256{},
                   /*congestion=*/static_cast<uint32_t>(8 * CONGESTION_ONE))};
    BOOST_REQUIRE(chain.AcceptHeader(anchor_header).accepted());
    const CBlockIndex& anchor{chain.Tip()};
    BOOST_REQUIRE_EQUAL(anchor.m_congestion, 8 * CONGESTION_ONE);

    const CKey funding_key{GenerateRandomKey()};
    auto context{agent::ImportAllotmentBundle(
        AgentAllotmentBundleJson(funding_key),
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash().ToString())};
    BOOST_REQUIRE_MESSAGE(context, util::ErrorString(context).original);

    const CKey destination_key{GenerateRandomKey()};
    auto signed_spend{agent::CreateSignedAllotmentSpend(
        *context,
        agent::AllotmentSpendRequest{
            .prevout = COutPoint{Txid::FromUint256(ArithToUint256(71)), 0},
            .prevout_value = COIN,
            .destination = PKHash(destination_key.GetPubKey()),
            .spend_amount = COIN / 4,
            .spent_today = 0,
            .prove = true,
            .anchor = &anchor,
        },
        consensus)};
    BOOST_REQUIRE_MESSAGE(signed_spend, util::ErrorString(signed_spend).original);

    const CTransaction& tx{signed_spend->transaction};
    BOOST_CHECK_EQUAL(tx.nAnchorHeight, static_cast<uint32_t>(anchor.nHeight));
    BOOST_CHECK(CheckTxProofOfWork(
        tx,
        GetTxPowTarget(consensus, &anchor, tx),
        anchor.GetBlockHash(),
        consensus));
}

// F-51, inverted. This case used to assert that a thin header carried NO congestion
// state (m_congestion == 0) and that proving therefore had to refuse. nCongestion is a
// header field now, so the premise is false by design: a header-only chain carries the
// multiplier, and proving at a non-genesis anchor must SUCCEED.
BOOST_AUTO_TEST_CASE(agent_allotment_proof_uses_header_sourced_congestion_at_thin_anchor)
{
    agent::HeaderChain chain{Params().GetConsensus(), Params().GenesisBlock()};
    const CBlockHeader header{NextHeader(chain)};
    BOOST_REQUIRE(chain.AcceptHeader(header).accepted());
    // The index entry takes the multiplier straight from the header it was built from.
    BOOST_REQUIRE_EQUAL(chain.Tip().m_congestion, header.nCongestion);
    BOOST_REQUIRE_EQUAL(chain.Tip().m_congestion, CONGESTION_ONE);
    BOOST_REQUIRE_GT(chain.Tip().nHeight, 0);  // past genesis: the case F-51 refused

    const CKey funding_key{GenerateRandomKey()};
    auto context{agent::ImportAllotmentBundle(
        AgentAllotmentBundleJson(funding_key),
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash().ToString())};
    BOOST_REQUIRE_MESSAGE(context, util::ErrorString(context).original);

    const CKey destination_key{GenerateRandomKey()};
    auto signed_spend{agent::CreateSignedAllotmentSpend(
        *context,
        agent::AllotmentSpendRequest{
            .prevout = COutPoint{Txid::FromUint256(ArithToUint256(72)), 0},
            .prevout_value = COIN,
            .destination = PKHash(destination_key.GetPubKey()),
            .spend_amount = COIN / 4,
            .spent_today = 0,
            .prove = true,
            .anchor = &chain.Tip(),
        },
        Params().GetConsensus())};

    BOOST_REQUIRE_MESSAGE(signed_spend, util::ErrorString(signed_spend).original);

    // ...and the proof it produced is one consensus accepts at that anchor.
    const CTransaction& tx{signed_spend->transaction};
    BOOST_CHECK_EQUAL(tx.nAnchorHeight, static_cast<uint32_t>(chain.Tip().nHeight));
    BOOST_CHECK(CheckTxProofOfWork(
        tx,
        GetTxPowTarget(Params().GetConsensus(), &chain.Tip(), tx),
        chain.Tip().GetBlockHash(),
        Params().GetConsensus()));
}

BOOST_AUTO_TEST_CASE(agent_allotment_selects_bundle_outputs_and_signs_spend)
{
    const CKey funding_key{GenerateRandomKey()};
    const std::vector<agent::AllotmentFundingOutputArtifact> funding_outputs{
        {
            .txid = Txid::FromUint256(ArithToUint256(31)).ToString(),
            .vout = 0,
            .amount = COIN / 5,
        },
        {
            .txid = Txid::FromUint256(ArithToUint256(32)).ToString(),
            .vout = 2,
            .amount = COIN / 3,
        },
    };
    auto context{agent::ImportAllotmentBundle(
        AgentAllotmentBundleJson(funding_key, /*funding_available=*/COIN, /*daily_limit=*/COIN, funding_outputs),
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash().ToString())};
    BOOST_REQUIRE_MESSAGE(context, util::ErrorString(context).original);

    const CKey destination_key{GenerateRandomKey()};
    const CAmount spend_amount{COIN / 2};
    const agent::AllotmentBundleSpendRequest request{
        .destination = PKHash(destination_key.GetPubKey()),
        .spend_amount = spend_amount,
        .spent_today = 0,
        .prove = false,
    };

    auto signed_spend{agent::CreateSignedAllotmentSpendFromBundleOutputs(*context, request, Params().GetConsensus())};
    BOOST_REQUIRE_MESSAGE(signed_spend, util::ErrorString(signed_spend).original);

    const CTransaction& tx{signed_spend->transaction};
    BOOST_REQUIRE_EQUAL(signed_spend->inputs.size(), 2U);
    BOOST_REQUIRE_EQUAL(tx.vin.size(), 2U);
    BOOST_REQUIRE_EQUAL(tx.vout.size(), 2U);
    BOOST_CHECK_EQUAL(signed_spend->input_amount, funding_outputs[0].amount + funding_outputs[1].amount);
    BOOST_CHECK_EQUAL(signed_spend->change_amount, signed_spend->input_amount - spend_amount);
    BOOST_CHECK_EQUAL(tx.vout[0].nValue, spend_amount);
    BOOST_CHECK(tx.vout[1].scriptPubKey == context->funding_script);

    for (size_t i{0}; i < tx.vin.size(); ++i) {
        ScriptError script_error{SCRIPT_ERR_OK};
        BOOST_CHECK(VerifyScript(tx.vin[i].scriptSig,
                                 context->funding_script,
                                 &tx.vin[i].scriptWitness,
                                 STANDARD_SCRIPT_VERIFY_FLAGS,
                                 TransactionSignatureChecker(&tx, i, signed_spend->inputs[i].amount, MissingDataBehavior::FAIL),
                                 &script_error));
        BOOST_CHECK_EQUAL(script_error, SCRIPT_ERR_OK);
    }
}

BOOST_AUTO_TEST_CASE(agent_allotment_rejects_insufficient_bundle_outputs)
{
    const CKey funding_key{GenerateRandomKey()};
    const std::vector<agent::AllotmentFundingOutputArtifact> funding_outputs{
        {
            .txid = Txid::FromUint256(ArithToUint256(41)).ToString(),
            .vout = 0,
            .amount = COIN / 10,
        },
    };
    auto context{agent::ImportAllotmentBundle(
        AgentAllotmentBundleJson(funding_key, /*funding_available=*/COIN, /*daily_limit=*/COIN, funding_outputs),
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash().ToString())};
    BOOST_REQUIRE_MESSAGE(context, util::ErrorString(context).original);

    const CKey destination_key{GenerateRandomKey()};
    auto signed_spend{agent::CreateSignedAllotmentSpendFromBundleOutputs(
        *context,
        agent::AllotmentBundleSpendRequest{
            .destination = PKHash(destination_key.GetPubKey()),
            .spend_amount = COIN / 2,
            .spent_today = 0,
            .prove = false,
        },
        Params().GetConsensus())};

    BOOST_CHECK(!signed_spend);
    BOOST_CHECK_EQUAL(util::ErrorString(signed_spend).original, "Agent allotment bundle does not include enough spendable funding outputs for this spend.");
}

BOOST_AUTO_TEST_CASE(agent_allotment_signing_rejects_policy_denied_spend)
{
    const CKey funding_key{GenerateRandomKey()};
    auto context{agent::ImportAllotmentBundle(
        AgentAllotmentBundleJson(funding_key, /*funding_available=*/COIN, /*daily_limit=*/COIN / 4),
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash().ToString())};
    BOOST_REQUIRE_MESSAGE(context, util::ErrorString(context).original);

    const CKey destination_key{GenerateRandomKey()};
    const agent::AllotmentSpendRequest request{
        .prevout = COutPoint{Txid::FromUint256(ArithToUint256(12)), 0},
        .prevout_value = COIN,
        .destination = PKHash(destination_key.GetPubKey()),
        .spend_amount = COIN / 2,
        .spent_today = 0,
        .prove = false,
    };

    auto signed_spend{agent::CreateSignedAllotmentSpend(*context, request, Params().GetConsensus())};
    BOOST_CHECK(!signed_spend);
    BOOST_CHECK_EQUAL(util::ErrorString(signed_spend).original, "Agent allotment policy rejected spend: daily-limit-exceeded");
}

BOOST_AUTO_TEST_CASE(agent_allotment_import_rejects_mismatched_bundle_key)
{
    const CKey funding_key{GenerateRandomKey()};
    std::string bundle{AgentAllotmentBundleJson(funding_key)};

    const CKey wrong_key{GenerateRandomKey()};
    const std::string funding_secret{EncodeSecret(funding_key)};
    const size_t secret_pos{bundle.find(funding_secret)};
    BOOST_REQUIRE_NE(secret_pos, std::string::npos);
    bundle.replace(secret_pos, funding_secret.size(), EncodeSecret(wrong_key));

    auto context{agent::ImportAllotmentBundle(
        bundle,
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash().ToString())};
    BOOST_CHECK(!context);
    BOOST_CHECK_EQUAL(util::ErrorString(context).original, "funding_secret_wif does not match the bundle funding address");
}

BOOST_AUTO_TEST_CASE(apply_payment_receipts_drops_spent_bundle_outputs)
{
    const CKey funding_key{GenerateRandomKey()};
    const std::string funding_address{EncodeDestination(WitnessV0KeyHash(funding_key.GetPubKey()))};
    const agent::AllotmentFundingOutputArtifact original{
        .txid = Txid::FromUint256(ArithToUint256(51)).ToString(),
        .vout = 0,
        .amount = COIN / 2,
    };
    const agent::AllotmentFundingOutputArtifact change{
        .txid = Txid::FromUint256(ArithToUint256(52)).ToString(),
        .vout = 1,
        .amount = COIN / 4,
    };

    auto context{agent::ImportAllotmentBundle(
        AgentAllotmentBundleJson(funding_key, /*funding_available=*/COIN / 2, /*daily_limit=*/COIN, {original}),
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash().ToString())};
    BOOST_REQUIRE_MESSAGE(context, util::ErrorString(context).original);

    const agent::AllotmentPaymentReceiptArtifact change_receipt{
        .chain = Params().GetChainTypeString(),
        .genesis_hash = Params().GenesisBlock().GetHash().ToString(),
        .funding_address = funding_address,
        .funding_output = change,
        .received_time = 2000,
        .payment_id = {},
        .label = {},
        .memo = {},
        .payer = {},
    };
    const std::vector<agent::AllotmentReceiptActivity> activities{
        {
            .type = agent::AllotmentReceiptActivityType::SPENT,
            .funding_address = funding_address,
            .funding_output = original,
            .event_time = 2000,
            .related_txid = change.txid,
            .payment_id = {},
            .label = {},
            .memo = {},
            .payer = {},
        },
        {
            .type = agent::AllotmentReceiptActivityType::CHANGE,
            .funding_address = funding_address,
            .funding_output = change,
            .event_time = 2000,
            .related_txid = change.txid,
            .payment_id = {},
            .label = {},
            .memo = {},
            .payer = {},
        },
    };

    BOOST_REQUIRE(agent::ApplyPaymentReceiptsToBundle(context->bundle, {change_receipt}, activities));
    BOOST_REQUIRE_EQUAL(context->bundle.funding_outputs.size(), 1U);
    BOOST_CHECK_EQUAL(context->bundle.funding_outputs[0].txid, change.txid);
    BOOST_CHECK_EQUAL(context->bundle.funding_outputs[0].vout, 1U);
    BOOST_CHECK_EQUAL(context->bundle.policy_request.funding_available, change.amount);

    const CKey destination_key{GenerateRandomKey()};
    auto signed_spend{agent::CreateSignedAllotmentSpendFromBundleOutputs(
        *context,
        agent::AllotmentBundleSpendRequest{
            .destination = PKHash(destination_key.GetPubKey()),
            .spend_amount = COIN / 4,
            .spent_today = 0,
            .prove = false,
        },
        Params().GetConsensus())};
    BOOST_REQUIRE_MESSAGE(signed_spend, util::ErrorString(signed_spend).original);
    BOOST_REQUIRE_EQUAL(signed_spend->inputs.size(), 1U);
    BOOST_CHECK_EQUAL(signed_spend->inputs[0].prevout.hash.ToString(), change.txid);
    BOOST_CHECK_EQUAL(signed_spend->inputs[0].prevout.n, 1U);
}

BOOST_AUTO_TEST_CASE(apply_payment_receipts_without_spent_filter_keeps_dead_output)
{
    const CKey funding_key{GenerateRandomKey()};
    const agent::AllotmentFundingOutputArtifact original{
        .txid = Txid::FromUint256(ArithToUint256(53)).ToString(),
        .vout = 0,
        .amount = COIN / 2,
    };
    const agent::AllotmentFundingOutputArtifact change{
        .txid = Txid::FromUint256(ArithToUint256(54)).ToString(),
        .vout = 1,
        .amount = COIN / 4,
    };
    auto context{agent::ImportAllotmentBundle(
        AgentAllotmentBundleJson(funding_key, /*funding_available=*/COIN / 2, /*daily_limit=*/COIN, {original}),
        Params().GetChainTypeString(),
        Params().GenesisBlock().GetHash().ToString())};
    BOOST_REQUIRE_MESSAGE(context, util::ErrorString(context).original);

    const agent::AllotmentPaymentReceiptArtifact change_receipt{
        .chain = Params().GetChainTypeString(),
        .genesis_hash = Params().GenesisBlock().GetHash().ToString(),
        .funding_address = context->bundle.funding_address,
        .funding_output = change,
        .received_time = 2000,
        .payment_id = {},
        .label = {},
        .memo = {},
        .payer = {},
    };
    // Passing empty activities is the pre-fix merge: original stays, change is unioned,
    // and greedy selection spends the dead UTXO first.
    BOOST_REQUIRE(agent::ApplyPaymentReceiptsToBundle(context->bundle, {change_receipt}, {}));
    BOOST_REQUIRE_EQUAL(context->bundle.funding_outputs.size(), 2U);

    const CKey destination_key{GenerateRandomKey()};
    auto signed_spend{agent::CreateSignedAllotmentSpendFromBundleOutputs(
        *context,
        agent::AllotmentBundleSpendRequest{
            .destination = PKHash(destination_key.GetPubKey()),
            .spend_amount = COIN / 4,
            .spent_today = 0,
            .prove = false,
        },
        Params().GetConsensus())};
    BOOST_REQUIRE_MESSAGE(signed_spend, util::ErrorString(signed_spend).original);
    BOOST_REQUIRE_EQUAL(signed_spend->inputs.size(), 1U);
    BOOST_CHECK_EQUAL(signed_spend->inputs[0].prevout.hash.ToString(), original.txid);
}

BOOST_AUTO_TEST_CASE(spent_today_sums_spend_amount_not_consumed_input)
{
    const std::string funding_address{"agent-address"};
    const std::string spend_txid{Txid::FromUint256(ArithToUint256(80)).ToString()};
    const std::vector<agent::AllotmentReceiptActivity> activities{
        {
            .type = agent::AllotmentReceiptActivityType::SPENT,
            .funding_address = funding_address,
            .funding_output = {.txid = Txid::FromUint256(ArithToUint256(81)).ToString(), .vout = 0, .amount = COIN},
            .event_time = 1'700'000'000,
            .related_txid = spend_txid,
            .payment_id = {},
            .label = {},
            .memo = {},
            .payer = {},
        },
        {
            .type = agent::AllotmentReceiptActivityType::CHANGE,
            .funding_address = funding_address,
            .funding_output = {.txid = spend_txid, .vout = 1, .amount = COIN / 2},
            .event_time = 1'700'000'000,
            .related_txid = spend_txid,
            .payment_id = {},
            .label = {},
            .memo = {},
            .payer = {},
        },
        {
            .type = agent::AllotmentReceiptActivityType::SPENT,
            .funding_address = funding_address,
            .funding_output = {.txid = Txid::FromUint256(ArithToUint256(82)).ToString(), .vout = 0, .amount = COIN / 10},
            .event_time = 1'700'000'000 - 86400,
            .related_txid = Txid::FromUint256(ArithToUint256(83)).ToString(),
            .payment_id = {},
            .label = {},
            .memo = {},
            .payer = {},
        },
    };

    auto today{agent::SpentTodayFromActivities(activities, funding_address, 1'700'000'000)};
    BOOST_REQUIRE_MESSAGE(today, util::ErrorString(today).original);
    BOOST_CHECK_EQUAL(*today, COIN / 2);

    auto yesterday{agent::SpentTodayFromActivities(activities, funding_address, 1'700'000'000 - 86400)};
    BOOST_REQUIRE_MESSAGE(yesterday, util::ErrorString(yesterday).original);
    BOOST_CHECK_EQUAL(*yesterday, COIN / 10);

    auto other_address{agent::SpentTodayFromActivities(activities, "other-address", 1'700'000'000)};
    BOOST_REQUIRE_MESSAGE(other_address, util::ErrorString(other_address).original);
    BOOST_CHECK_EQUAL(*other_address, 0);
}

BOOST_AUTO_TEST_SUITE_END()
