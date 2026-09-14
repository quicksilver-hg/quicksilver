// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef QUICKSILVER_VAULT_ARCHIVE_H
#define QUICKSILVER_VAULT_ARCHIVE_H

#include <primitives/transaction.h>
#include <support/allocators/secure.h>
#include <uint256.h>
#include <util/fs.h>
#include <util/result.h>

#include <cstdint>
#include <string>
#include <vector>

namespace vault {

class CVault;

/** A vault archive is the ONLY record of a thin vault's transaction history.
 *
 * A thin vault cannot rescan, so the chain is not the backup of last resort here:
 * `scantxoutset` can rediscover spendable outputs for keys the vault still has, but
 * it cannot recover lost keys or what was sent, to whom, or when. That makes both
 * the vault file and its history things the user has to carry deliberately.
 *
 * The archive therefore carries the descriptors (public only), the vault's birth time,
 * and the full transaction history with its confirmation state. It carries no private
 * keys: keys are the vault-file backup's job, and an archive that held them would turn
 * a history backup into a second copy of the vault's secrets.
 *
 * It is encrypted regardless, because a complete payment history is disclosive even
 * without the ability to spend.
 */

//! Bumped when the archive body's layout changes.
static constexpr uint32_t VAULT_ARCHIVE_VERSION{1};

/** One historical transaction and the chain position it was last seen at. */
struct VaultArchiveTx {
    CTransactionRef tx;
    uint32_t time_received{0};
    uint32_t time_smart{0};
    bool from_me{false};
    int64_t order_pos{-1};
    //! Null when the transaction was not confirmed at export time.
    uint256 block_hash;
    int32_t block_height{0};
    int32_t block_index{0};
    bool confirmed{false};
    std::vector<std::pair<std::string, std::string>> value_map;
};

/** One descriptor, public form only. */
struct VaultArchiveDescriptor {
    std::string descriptor;
    uint64_t creation_time{0};
    bool active{false};
    bool internal{false};
    int64_t range_start{0};
    int64_t range_end{0};
    bool is_range{false};
    int64_t next_index{0};
};

/** One address book entry.
 *
 * This is not decoration. The presence or absence of a label is what distinguishes a
 * change address from a real one (see CAddressBookData), so a restored vault without an
 * address book treats every address as change — and a self-payment then produces no
 * history rows at all. Labels are also the only record of who was paid. */
struct VaultArchiveAddress {
    std::string destination;
    bool has_label{false};
    std::string label;
    //! 0 = unset, otherwise AddressPurpose + 1, so "no purpose recorded" survives the trip.
    uint8_t purpose{0};
};

/** The decrypted body of an archive. */
struct VaultArchive {
    uint32_t version{VAULT_ARCHIVE_VERSION};
    std::string chain;
    uint256 genesis_hash;
    std::string vault_name;
    int64_t birth_time{0};
    int64_t created_time{0};
    std::vector<VaultArchiveDescriptor> descriptors;
    std::vector<VaultArchiveAddress> addresses;
    std::vector<VaultArchiveTx> transactions;
};

/** What an import actually changed, so the caller can report it rather than guess. */
struct VaultArchiveImportResult {
    size_t descriptors_imported{0};
    size_t addresses_imported{0};
    size_t transactions_imported{0};
    size_t transactions_skipped{0};
};

/** Collect the archive body from a live vault. Does not touch the filesystem. */
util::Result<VaultArchive> BuildVaultArchive(const CVault& vault);

/** Encrypt and write an archive. Writes to a temporary file and renames, so a failed
 *  export never leaves a truncated archive where a good one was. */
util::Result<void> WriteVaultArchive(const VaultArchive& archive,
                                     const fs::path& path,
                                     const SecureString& passphrase);

/** Read and decrypt an archive. A wrong passphrase is reported as such rather than as
 *  a corrupt file, because those need different things from the user. */
util::Result<VaultArchive> ReadVaultArchive(const fs::path& path,
                                            const SecureString& passphrase);

/** Load an archive's descriptors and history into a vault.
 *
 * The vault must be for the same chain the archive came from. Transactions already
 * present are skipped rather than overwritten, so importing twice is harmless. */
util::Result<VaultArchiveImportResult> ImportVaultArchive(CVault& vault, const VaultArchive& archive);

} // namespace vault

#endif // QUICKSILVER_VAULT_ARCHIVE_H
