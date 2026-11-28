// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef QUICKSILVER_AGENT_ALLOTMENTSTORE_H
#define QUICKSILVER_AGENT_ALLOTMENTSTORE_H

#include <agent/allotmentpolicy.h>
#include <uint256.h>
#include <util/fs.h>
#include <util/fs_helpers.h>
#include <util/result.h>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace agent {

//! The agent's day: whole UTC days since the epoch. The agent's daily limit
//! (SpentTodayFromActivities) and the vault's (F-661, vault/spend.cpp) share it.
inline constexpr int64_t SECONDS_PER_UTC_DAY{86400};

inline constexpr uint64_t MAX_ALLOTMENT_RECEIPTS{1'000'000};
inline constexpr uint64_t MAX_ALLOTMENT_RECEIPT_ACTIVITIES{2'000'000};

//! The record caps a receipt store is saved and loaded under. Save refuses what
//! load would refuse, so a store that saves always loads. Tests pass smaller caps.
struct AllotmentStoreLimits {
    uint64_t max_receipts{MAX_ALLOTMENT_RECEIPTS};
    uint64_t max_activities{MAX_ALLOTMENT_RECEIPT_ACTIVITIES};
};

enum class AllotmentStoreResult {
    OK,
    FILE_NOT_FOUND,
    FILE_OPEN_FAILED,
    FILE_WRITE_FAILED,
    FILE_COMMIT_FAILED,
    FILE_RENAME_FAILED,
    DESERIALIZE_FAILED,
    BAD_MAGIC,
    BAD_VERSION,
    BAD_ACTIVITY,
    BAD_GENESIS,
    TOO_MANY_RECEIPTS,
    TOO_MANY_ACTIVITIES,
};

enum class AllotmentReceiptActivityType : uint8_t {
    IMPORTED = 0,
    SPENT = 1,
    CHANGE = 2,
};

struct AllotmentReceiptActivity {
    AllotmentReceiptActivityType type{AllotmentReceiptActivityType::IMPORTED};
    std::string funding_address;
    AllotmentFundingOutputArtifact funding_output;
    int64_t event_time{0};
    std::string related_txid;
    std::string payment_id;
    std::string label;
    std::string memo;
    std::string payer;
};

struct AllotmentReceiptStoreData {
    std::vector<AllotmentPaymentReceiptArtifact> receipts;
    std::vector<AllotmentReceiptActivity> activities;
};

struct AllotmentReceiptStoreLoadResult {
    AllotmentStoreResult status;
    std::vector<AllotmentPaymentReceiptArtifact> receipts;
    std::vector<AllotmentReceiptActivity> activities;

    bool ok() const { return status == AllotmentStoreResult::OK; }
};

struct AllotmentReceiptDirectoryScanResult {
    size_t scanned_files{0};
    size_t imported_receipts{0};
    AllotmentReceiptStoreData store;
};

const char* AllotmentStoreResultString(AllotmentStoreResult status);
const char* AllotmentReceiptActivityTypeString(AllotmentReceiptActivityType type);

/** Hold the receipt store directory lock until the process exits. */
util::LockResult LockReceiptStoreDirectory(const fs::path& directory);

//! Writes a uniquely named temp file beside `path` and renames it over `path`; the
//! temp file is removed on any failure. TOO_MANY_RECEIPTS / TOO_MANY_ACTIVITIES
//! when the data exceeds `limits`, before anything is written.
AllotmentStoreResult SaveAllotmentReceiptStore(const AllotmentReceiptStoreData& data,
                                         const fs::path& path,
                                         const uint256& genesis_hash,
                                         const AllotmentStoreLimits& limits = {});
AllotmentStoreResult SaveAllotmentPaymentReceipts(const std::vector<AllotmentPaymentReceiptArtifact>& receipts,
                                            const fs::path& path,
                                            const uint256& genesis_hash);
//! FILE_OPEN_FAILED, not an exception, when `path` cannot even be probed.
AllotmentReceiptStoreLoadResult LoadAllotmentPaymentReceipts(const fs::path& path,
                                                       const uint256& expected_genesis_hash,
                                                       const AllotmentStoreLimits& limits = {});
util::Result<AllotmentReceiptStoreData> LoadAllotmentReceiptStore(const fs::path& path,
                                                            std::string_view expected_chain,
                                                            const uint256& expected_genesis_hash);
util::Result<size_t> AddAllotmentPaymentReceipts(std::vector<AllotmentPaymentReceiptArtifact>& stored_receipts,
                                              const std::vector<AllotmentPaymentReceiptArtifact>& receipts,
                                              std::vector<AllotmentPaymentReceiptArtifact>* added_receipts = nullptr);
util::Result<size_t> AddAllotmentPaymentReceipts(AllotmentReceiptStoreData& store,
                                              const std::vector<AllotmentPaymentReceiptArtifact>& receipts,
                                              std::vector<AllotmentPaymentReceiptArtifact>* added_receipts = nullptr);
util::Result<AllotmentReceiptDirectoryScanResult> ScanAllotmentPaymentReceiptDirectory(const fs::path& directory,
                                                                                 const fs::path& store_path,
                                                                                 std::string_view expected_chain,
                                                                                 const uint256& expected_genesis_hash);

//! Drop consumed outpoints from a bundle and union remaining receipts onto it.
//! Spent outpoints are taken from SPENT activity, not from the live receipt list,
//! so a desktop-exported bundle cannot resurrect a UTXO the ledger already spent.
util::Result<void> ApplyPaymentReceiptsToBundle(AllotmentPolicyBundleArtifact& bundle,
                                                const std::vector<AllotmentPaymentReceiptArtifact>& receipts,
                                                const std::vector<AllotmentReceiptActivity>& activities);

//! Sum today's allotment spends from the receipt ledger. A spend is the SPENT
//! input total minus matching CHANGE for the same related_txid, so change is
//! not counted against the daily guardrail. `now` is unix time; "today" is the
//! UTC calendar day. Activities for other funding addresses are ignored.
util::Result<CAmount> SpentTodayFromActivities(const std::vector<AllotmentReceiptActivity>& activities,
                                               std::string_view funding_address,
                                               int64_t now);

size_t RemoveSpentReceipts(std::vector<AllotmentPaymentReceiptArtifact>& receipts,
                           const std::vector<AllotmentFundingOutputArtifact>& spent_outputs,
                           std::vector<AllotmentPaymentReceiptArtifact>* removed_receipts = nullptr);

void AppendSpentActivities(AllotmentReceiptStoreData& store,
                           std::string_view funding_address,
                           const std::vector<AllotmentFundingOutputArtifact>& spent_outputs,
                           const std::vector<AllotmentPaymentReceiptArtifact>& spent_receipts,
                           int64_t event_time,
                           std::string_view related_txid);

} // namespace agent

#endif // QUICKSILVER_AGENT_ALLOTMENTSTORE_H
