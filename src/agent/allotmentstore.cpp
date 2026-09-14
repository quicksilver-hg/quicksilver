// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <agent/allotmentstore.h>

#include <consensus/amount.h>
#include <streams.h>
#include <tinyformat.h>
#include <util/fs_helpers.h>
#include <util/readwritefile.h>
#include <util/result.h>
#include <util/strencodings.h>
#include <util/translation.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace agent {
namespace {

constexpr uint32_t ALLOTMENT_RECEIPT_STORE_MAGIC{0x47525351}; // "QSRG" in little-endian streams.
constexpr uint32_t ALLOTMENT_RECEIPT_STORE_VERSION{3};
constexpr uint32_t ALLOTMENT_RECEIPT_STORE_MIN_VERSION{1};
constexpr uint64_t MAX_ALLOTMENT_RECEIPTS{1'000'000};
constexpr uint64_t MAX_ALLOTMENT_RECEIPT_ACTIVITIES{2'000'000};
constexpr size_t MAX_PAYMENT_RECEIPT_FILE_SIZE{1'000'000};
constexpr int64_t SECONDS_PER_UTC_DAY{86400};

using FundingOutpointKey = std::pair<std::string, uint32_t>;

std::set<FundingOutpointKey> SpentOutpoints(const std::vector<AllotmentReceiptActivity>& activities,
                                            std::string_view funding_address)
{
    std::set<FundingOutpointKey> spent;
    for (const AllotmentReceiptActivity& activity : activities) {
        if (activity.type != AllotmentReceiptActivityType::SPENT) continue;
        if (activity.funding_address != funding_address) continue;
        spent.emplace(activity.funding_output.txid, activity.funding_output.vout);
    }
    return spent;
}

AllotmentReceiptStoreLoadResult LoadResult(AllotmentStoreResult status,
                                        std::vector<AllotmentPaymentReceiptArtifact> receipts = {},
                                        std::vector<AllotmentReceiptActivity> activities = {})
{
    return {status, std::move(receipts), std::move(activities)};
}

AllotmentReceiptActivity ImportedActivity(const AllotmentPaymentReceiptArtifact& receipt)
{
    return {
        .type = AllotmentReceiptActivityType::IMPORTED,
        .funding_address = receipt.funding_address,
        .funding_output = receipt.funding_output,
        .event_time = receipt.received_time,
        .related_txid = {},
        .payment_id = receipt.payment_id,
        .label = receipt.label,
        .memo = receipt.memo,
        .payer = receipt.payer,
    };
}

} // namespace

const char* AllotmentStoreResultString(AllotmentStoreResult status)
{
    switch (status) {
    case AllotmentStoreResult::OK:
        return "ok";
    case AllotmentStoreResult::FILE_NOT_FOUND:
        return "file-not-found";
    case AllotmentStoreResult::FILE_OPEN_FAILED:
        return "file-open-failed";
    case AllotmentStoreResult::FILE_WRITE_FAILED:
        return "file-write-failed";
    case AllotmentStoreResult::FILE_COMMIT_FAILED:
        return "file-commit-failed";
    case AllotmentStoreResult::FILE_RENAME_FAILED:
        return "file-rename-failed";
    case AllotmentStoreResult::DESERIALIZE_FAILED:
        return "deserialize-failed";
    case AllotmentStoreResult::BAD_MAGIC:
        return "bad-magic";
    case AllotmentStoreResult::BAD_VERSION:
        return "bad-version";
    case AllotmentStoreResult::BAD_GENESIS:
        return "bad-genesis";
    case AllotmentStoreResult::TOO_MANY_RECEIPTS:
        return "too-many-receipts";
    case AllotmentStoreResult::TOO_MANY_ACTIVITIES:
        return "too-many-activities";
    } // no default case, so the compiler can warn on missing enum values
    assert(false);
    return "unknown";
}

const char* AllotmentReceiptActivityTypeString(AllotmentReceiptActivityType type)
{
    switch (type) {
    case AllotmentReceiptActivityType::IMPORTED:
        return "imported";
    case AllotmentReceiptActivityType::SPENT:
        return "spent";
    case AllotmentReceiptActivityType::CHANGE:
        return "change";
    } // no default case, so the compiler can warn on missing enum values
    assert(false);
    return "unknown";
}

AllotmentStoreResult SaveAllotmentReceiptStore(const AllotmentReceiptStoreData& data,
                                         const fs::path& path,
                                         const uint256& genesis_hash)
{
    const fs::path temp_path{path + ".new"};
    AutoFile file{fsbridge::fopen(temp_path, "wb")};
    if (file.IsNull()) return AllotmentStoreResult::FILE_OPEN_FAILED;

    try {
        file << ALLOTMENT_RECEIPT_STORE_MAGIC;
        file << ALLOTMENT_RECEIPT_STORE_VERSION;
        file << genesis_hash;
        file << static_cast<uint64_t>(data.receipts.size());
        for (const AllotmentPaymentReceiptArtifact& receipt : data.receipts) {
            file << receipt.chain;
            file << receipt.genesis_hash;
            file << receipt.funding_address;
            file << receipt.funding_output.txid;
            file << receipt.funding_output.vout;
            file << receipt.funding_output.amount;
            file << receipt.received_time;
            file << receipt.payment_id;
            file << receipt.label;
            file << receipt.memo;
            file << receipt.payer;
        }
        file << static_cast<uint64_t>(data.activities.size());
        for (const AllotmentReceiptActivity& activity : data.activities) {
            const uint8_t type{static_cast<uint8_t>(activity.type)};
            file << type;
            file << activity.funding_address;
            file << activity.funding_output.txid;
            file << activity.funding_output.vout;
            file << activity.funding_output.amount;
            file << activity.event_time;
            file << activity.related_txid;
            file << activity.payment_id;
            file << activity.label;
            file << activity.memo;
            file << activity.payer;
        }

        if (!file.Commit()) return AllotmentStoreResult::FILE_COMMIT_FAILED;
        file.fclose();
        if (!RenameOver(temp_path, path)) return AllotmentStoreResult::FILE_RENAME_FAILED;
    } catch (const std::exception&) {
        return AllotmentStoreResult::FILE_WRITE_FAILED;
    }
    return AllotmentStoreResult::OK;
}

AllotmentStoreResult SaveAllotmentPaymentReceipts(const std::vector<AllotmentPaymentReceiptArtifact>& receipts,
                                            const fs::path& path,
                                            const uint256& genesis_hash)
{
    return SaveAllotmentReceiptStore({.receipts = receipts, .activities = {}}, path, genesis_hash);
}

AllotmentReceiptStoreLoadResult LoadAllotmentPaymentReceipts(const fs::path& path,
                                                       const uint256& expected_genesis_hash)
{
    if (path.empty() || !fs::exists(path)) {
        return LoadResult(AllotmentStoreResult::FILE_NOT_FOUND);
    }

    AutoFile file{fsbridge::fopen(path, "rb")};
    if (file.IsNull()) return LoadResult(AllotmentStoreResult::FILE_OPEN_FAILED);

    try {
        uint32_t magic{0};
        uint32_t version{0};
        uint256 stored_genesis;
        uint64_t receipt_count{0};

        file >> magic;
        file >> version;
        file >> stored_genesis;
        file >> receipt_count;

        if (magic != ALLOTMENT_RECEIPT_STORE_MAGIC) return LoadResult(AllotmentStoreResult::BAD_MAGIC);
        if (version < ALLOTMENT_RECEIPT_STORE_MIN_VERSION || version > ALLOTMENT_RECEIPT_STORE_VERSION) return LoadResult(AllotmentStoreResult::BAD_VERSION);
        if (stored_genesis != expected_genesis_hash) return LoadResult(AllotmentStoreResult::BAD_GENESIS);
        if (receipt_count > MAX_ALLOTMENT_RECEIPTS) return LoadResult(AllotmentStoreResult::TOO_MANY_RECEIPTS);

        std::vector<AllotmentPaymentReceiptArtifact> receipts;
        receipts.reserve(receipt_count);
        for (uint64_t i{0}; i < receipt_count; ++i) {
            AllotmentPaymentReceiptArtifact receipt;
            file >> receipt.chain;
            file >> receipt.genesis_hash;
            file >> receipt.funding_address;
            file >> receipt.funding_output.txid;
            file >> receipt.funding_output.vout;
            file >> receipt.funding_output.amount;
            file >> receipt.received_time;
            if (version >= 3) {
                file >> receipt.payment_id;
                file >> receipt.label;
                file >> receipt.memo;
                file >> receipt.payer;
            }
            receipts.push_back(std::move(receipt));
        }
        std::vector<AllotmentReceiptActivity> activities;
        if (version >= 2) {
            uint64_t activity_count{0};
            file >> activity_count;
            if (activity_count > MAX_ALLOTMENT_RECEIPT_ACTIVITIES) return LoadResult(AllotmentStoreResult::TOO_MANY_ACTIVITIES);
            activities.reserve(activity_count);
            for (uint64_t i{0}; i < activity_count; ++i) {
                uint8_t type{0};
                AllotmentReceiptActivity activity;
                file >> type;
                file >> activity.funding_address;
                file >> activity.funding_output.txid;
                file >> activity.funding_output.vout;
                file >> activity.funding_output.amount;
                file >> activity.event_time;
                file >> activity.related_txid;
                if (version >= 3) {
                    file >> activity.payment_id;
                    file >> activity.label;
                    file >> activity.memo;
                    file >> activity.payer;
                }
                if (type == static_cast<uint8_t>(AllotmentReceiptActivityType::SPENT)) {
                    activity.type = AllotmentReceiptActivityType::SPENT;
                } else if (type == static_cast<uint8_t>(AllotmentReceiptActivityType::CHANGE)) {
                    activity.type = AllotmentReceiptActivityType::CHANGE;
                } else {
                    activity.type = AllotmentReceiptActivityType::IMPORTED;
                }
                activities.push_back(std::move(activity));
            }
        }
        return LoadResult(AllotmentStoreResult::OK, std::move(receipts), std::move(activities));
    } catch (const std::exception&) {
        return LoadResult(AllotmentStoreResult::DESERIALIZE_FAILED);
    }
}

util::Result<AllotmentReceiptStoreData> LoadAllotmentReceiptStore(const fs::path& path,
                                                            std::string_view expected_chain,
                                                            const uint256& expected_genesis_hash)
{
    AllotmentReceiptStoreLoadResult loaded{LoadAllotmentPaymentReceipts(path, expected_genesis_hash)};
    if (loaded.status == AllotmentStoreResult::FILE_NOT_FOUND) {
        return AllotmentReceiptStoreData{};
    }
    if (!loaded.ok()) {
        return util::Error{Untranslated(strprintf("Could not load receipt store %s: %s", fs::PathToString(path), AllotmentStoreResultString(loaded.status)))};
    }
    for (const AllotmentPaymentReceiptArtifact& receipt : loaded.receipts) {
        if (receipt.chain != expected_chain || receipt.genesis_hash != expected_genesis_hash.ToString()) {
            return util::Error{Untranslated("Agent allotment receipt store contains a receipt for a different chain.")};
        }
    }
    return AllotmentReceiptStoreData{
        .receipts = std::move(loaded.receipts),
        .activities = std::move(loaded.activities),
    };
}

util::Result<size_t> AddAllotmentPaymentReceipts(std::vector<AllotmentPaymentReceiptArtifact>& stored_receipts,
                                              const std::vector<AllotmentPaymentReceiptArtifact>& receipts,
                                              std::vector<AllotmentPaymentReceiptArtifact>* added_receipts)
{
    size_t added{0};
    for (const AllotmentPaymentReceiptArtifact& receipt : receipts) {
        const auto duplicate{std::find_if(stored_receipts.begin(), stored_receipts.end(), [&](const auto& stored) {
            return stored.funding_output.txid == receipt.funding_output.txid && stored.funding_output.vout == receipt.funding_output.vout;
        })};
        if (duplicate != stored_receipts.end()) {
            if (duplicate->funding_address != receipt.funding_address || duplicate->funding_output.amount != receipt.funding_output.amount) {
                return util::Error{Untranslated("Agent allotment payment receipt duplicates a stored funding output with different metadata.")};
            }
            continue;
        }
        stored_receipts.push_back(receipt);
        if (added_receipts) added_receipts->push_back(receipt);
        ++added;
    }
    return added;
}

util::Result<size_t> AddAllotmentPaymentReceipts(AllotmentReceiptStoreData& store,
                                              const std::vector<AllotmentPaymentReceiptArtifact>& receipts,
                                              std::vector<AllotmentPaymentReceiptArtifact>* added_receipts)
{
    std::vector<AllotmentPaymentReceiptArtifact> unspent_receipts;
    unspent_receipts.reserve(receipts.size());
    for (const AllotmentPaymentReceiptArtifact& receipt : receipts) {
        const auto spent{std::find_if(store.activities.begin(), store.activities.end(), [&](const AllotmentReceiptActivity& activity) {
            return activity.type == AllotmentReceiptActivityType::SPENT &&
                   activity.funding_output.txid == receipt.funding_output.txid &&
                   activity.funding_output.vout == receipt.funding_output.vout;
        })};
        if (spent == store.activities.end()) {
            unspent_receipts.push_back(receipt);
            continue;
        }
        if (spent->funding_address != receipt.funding_address || spent->funding_output.amount != receipt.funding_output.amount) {
            return util::Error{Untranslated("Agent allotment payment receipt duplicates a spent funding output with different metadata.")};
        }
    }
    return AddAllotmentPaymentReceipts(store.receipts, unspent_receipts, added_receipts);
}

util::Result<AllotmentReceiptDirectoryScanResult> ScanAllotmentPaymentReceiptDirectory(const fs::path& directory,
                                                                                 const fs::path& store_path,
                                                                                 std::string_view expected_chain,
                                                                                 const uint256& expected_genesis_hash)
{
    std::vector<fs::path> receipt_files;
    try {
        if (fs::exists(directory)) {
            if (!fs::is_directory(directory)) {
                return util::Error{Untranslated(strprintf("Payment receipt inbox %s is not a directory.", fs::PathToString(directory)))};
            }
            for (const auto& entry : fs::directory_iterator(directory)) {
                if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
                receipt_files.push_back(entry.path());
            }
            std::sort(receipt_files.begin(), receipt_files.end());
        }
    } catch (const fs::filesystem_error& error) {
        return util::Error{Untranslated(strprintf("Could not scan payment receipt inbox %s: %s", fs::PathToString(directory), error.what()))};
    }

    std::vector<AllotmentPaymentReceiptArtifact> decoded_receipts;
    decoded_receipts.reserve(receipt_files.size());
    for (const fs::path& path : receipt_files) {
        const auto [ok, contents]{ReadBinaryFile(path, MAX_PAYMENT_RECEIPT_FILE_SIZE)};
        if (!ok) {
            return util::Error{Untranslated(strprintf("Could not read payment receipt file %s.", fs::PathToString(path)))};
        }
        const std::string json{util::TrimString(contents)};
        if (json.empty()) {
            return util::Error{Untranslated(strprintf("Payment receipt file %s is empty.", fs::PathToString(path)))};
        }
        auto receipt{DecodeAllotmentPaymentReceipt(json, expected_chain, expected_genesis_hash.ToString())};
        if (!receipt) {
            return util::Error{Untranslated(strprintf("Payment receipt file %s: %s", fs::PathToString(path), util::ErrorString(receipt).original))};
        }
        decoded_receipts.push_back(std::move(*receipt));
    }

    auto store{LoadAllotmentReceiptStore(store_path, expected_chain, expected_genesis_hash)};
    if (!store) return util::Error{util::ErrorString(store)};

    std::vector<AllotmentPaymentReceiptArtifact> added_receipts;
    auto added{AddAllotmentPaymentReceipts(*store, decoded_receipts, &added_receipts)};
    if (!added) return util::Error{util::ErrorString(added)};

    for (const AllotmentPaymentReceiptArtifact& receipt : added_receipts) {
        store->activities.push_back(ImportedActivity(receipt));
    }
    if (*added > 0) {
        const fs::path parent_path{store_path.parent_path()};
        if (!parent_path.empty() && !fs::is_directory(parent_path) && !TryCreateDirectories(parent_path)) {
            return util::Error{Untranslated(strprintf("Could not create agent receipt store directory %s.", fs::PathToString(parent_path)))};
        }
        const AllotmentStoreResult saved{SaveAllotmentReceiptStore(*store, store_path, expected_genesis_hash)};
        if (saved != AllotmentStoreResult::OK) {
            return util::Error{Untranslated(strprintf("Could not write receipt store %s: %s", fs::PathToString(store_path), AllotmentStoreResultString(saved)))};
        }
    }

    return AllotmentReceiptDirectoryScanResult{
        .scanned_files = receipt_files.size(),
        .imported_receipts = *added,
        .store = std::move(*store),
    };
}

util::Result<void> ApplyPaymentReceiptsToBundle(AllotmentPolicyBundleArtifact& bundle,
                                                const std::vector<AllotmentPaymentReceiptArtifact>& receipts,
                                                const std::vector<AllotmentReceiptActivity>& activities)
{
    const std::set<FundingOutpointKey> spent{SpentOutpoints(activities, bundle.funding_address)};

    bundle.funding_outputs.erase(std::remove_if(bundle.funding_outputs.begin(), bundle.funding_outputs.end(),
                                                [&](const AllotmentFundingOutputArtifact& output) {
                                                    return spent.contains({output.txid, output.vout});
                                                }),
                                 bundle.funding_outputs.end());

    std::set<FundingOutpointKey> seen_outputs;
    for (const AllotmentFundingOutputArtifact& output : bundle.funding_outputs) {
        seen_outputs.emplace(output.txid, output.vout);
    }

    for (const AllotmentPaymentReceiptArtifact& receipt : receipts) {
        if (receipt.funding_address != bundle.funding_address) continue;
        const FundingOutpointKey key{receipt.funding_output.txid, receipt.funding_output.vout};
        if (spent.contains(key)) {
            const auto spent_activity{std::find_if(activities.begin(), activities.end(), [&](const AllotmentReceiptActivity& activity) {
                return activity.type == AllotmentReceiptActivityType::SPENT &&
                       activity.funding_address == receipt.funding_address &&
                       activity.funding_output.txid == receipt.funding_output.txid &&
                       activity.funding_output.vout == receipt.funding_output.vout;
            })};
            if (spent_activity != activities.end() &&
                (spent_activity->funding_output.amount != receipt.funding_output.amount)) {
                return util::Error{Untranslated("Agent allotment payment receipt duplicates a spent funding output with a different amount.")};
            }
            continue;
        }
        const auto duplicate{std::find_if(bundle.funding_outputs.begin(), bundle.funding_outputs.end(), [&](const auto& output) {
            return output.txid == receipt.funding_output.txid && output.vout == receipt.funding_output.vout;
        })};
        if (!seen_outputs.insert(key).second) {
            if (duplicate != bundle.funding_outputs.end() && duplicate->amount != receipt.funding_output.amount) {
                return util::Error{Untranslated("Agent allotment payment receipt duplicates a funding output with a different amount.")};
            }
            continue;
        }
        bundle.funding_outputs.push_back(receipt.funding_output);
    }

    CAmount funding_output_total{0};
    for (const AllotmentFundingOutputArtifact& output : bundle.funding_outputs) {
        if (!MoneyRange(funding_output_total + output.amount)) {
            return util::Error{Untranslated("Agent allotment payment receipt funding output total is out of range.")};
        }
        funding_output_total += output.amount;
    }
    if (funding_output_total > bundle.policy_request.policy.funding_limit) {
        return util::Error{Untranslated("Agent allotment payment receipt funding output total exceeds the policy funding limit.")};
    }
    bundle.policy_request.funding_available = funding_output_total;
    return {};
}

util::Result<CAmount> SpentTodayFromActivities(const std::vector<AllotmentReceiptActivity>& activities,
                                               std::string_view funding_address,
                                               int64_t now)
{
    const int64_t today{now / SECONDS_PER_UTC_DAY};

    struct SpendGroup {
        CAmount inputs{0};
        CAmount change{0};
        bool has_spent{false};
    };
    std::map<std::string, SpendGroup> by_spend;
    CAmount ungrouped{0};

    auto add_amount = [](CAmount& total, CAmount amount) -> util::Result<void> {
        if (amount < 0 || !MoneyRange(amount) || !MoneyRange(total + amount)) {
            return util::Error{Untranslated("Agent receipt ledger spent-today total is out of range.")};
        }
        total += amount;
        return {};
    };

    for (const AllotmentReceiptActivity& activity : activities) {
        if (activity.funding_address != funding_address) continue;
        if (activity.type == AllotmentReceiptActivityType::SPENT) {
            if (activity.event_time / SECONDS_PER_UTC_DAY != today) continue;
            if (activity.related_txid.empty()) {
                if (auto added{add_amount(ungrouped, activity.funding_output.amount)}; !added) {
                    return util::Error{util::ErrorString(added)};
                }
                continue;
            }
            SpendGroup& group{by_spend[activity.related_txid]};
            if (auto added{add_amount(group.inputs, activity.funding_output.amount)}; !added) {
                return util::Error{util::ErrorString(added)};
            }
            group.has_spent = true;
        } else if (activity.type == AllotmentReceiptActivityType::CHANGE && !activity.related_txid.empty()) {
            SpendGroup& group{by_spend[activity.related_txid]};
            if (auto added{add_amount(group.change, activity.funding_output.amount)}; !added) {
                return util::Error{util::ErrorString(added)};
            }
        }
    }

    CAmount spent_today{ungrouped};
    for (const auto& [txid, group] : by_spend) {
        if (!group.has_spent) continue;
        if (group.change > group.inputs) {
            return util::Error{Untranslated("Agent receipt ledger change exceeds spent inputs for a spend.")};
        }
        if (auto added{add_amount(spent_today, group.inputs - group.change)}; !added) {
            return util::Error{util::ErrorString(added)};
        }
    }
    return spent_today;
}

size_t RemoveSpentReceipts(std::vector<AllotmentPaymentReceiptArtifact>& receipts,
                           const std::vector<AllotmentFundingOutputArtifact>& spent_outputs,
                           std::vector<AllotmentPaymentReceiptArtifact>* removed_receipts)
{
    size_t removed{0};
    for (const AllotmentFundingOutputArtifact& output : spent_outputs) {
        const auto old_size{receipts.size()};
        if (removed_receipts) {
            for (const auto& receipt : receipts) {
                if (receipt.funding_output.txid == output.txid && receipt.funding_output.vout == output.vout) {
                    removed_receipts->push_back(receipt);
                }
            }
        }
        receipts.erase(std::remove_if(receipts.begin(), receipts.end(), [&](const auto& receipt) {
                           return receipt.funding_output.txid == output.txid && receipt.funding_output.vout == output.vout;
                       }),
                       receipts.end());
        removed += old_size - receipts.size();
    }
    return removed;
}

void AppendSpentActivities(AllotmentReceiptStoreData& store,
                           std::string_view funding_address,
                           const std::vector<AllotmentFundingOutputArtifact>& spent_outputs,
                           const std::vector<AllotmentPaymentReceiptArtifact>& spent_receipts,
                           int64_t event_time,
                           std::string_view related_txid)
{
    for (const AllotmentFundingOutputArtifact& output : spent_outputs) {
        const auto receipt{std::find_if(spent_receipts.begin(), spent_receipts.end(), [&](const auto& candidate) {
            return candidate.funding_output.txid == output.txid && candidate.funding_output.vout == output.vout;
        })};
        AllotmentReceiptActivity activity{
            .type = AllotmentReceiptActivityType::SPENT,
            .funding_address = std::string{funding_address},
            .funding_output = output,
            .event_time = event_time,
            .related_txid = std::string{related_txid},
            .payment_id = {},
            .label = {},
            .memo = {},
            .payer = {},
        };
        if (receipt != spent_receipts.end()) {
            activity.funding_address = receipt->funding_address;
            activity.funding_output = receipt->funding_output;
            activity.payment_id = receipt->payment_id;
            activity.label = receipt->label;
            activity.memo = receipt->memo;
            activity.payer = receipt->payer;
        }
        store.activities.push_back(std::move(activity));
    }
}

} // namespace agent
