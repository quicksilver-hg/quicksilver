// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <vault/archive.h>

#include <chainparams.h>
#include <hash.h>
#include <key_io.h>
#include <random.h>
#include <script/descriptor.h>
#include <script/signingprovider.h>
#include <streams.h>
#include <util/check.h>
#include <util/fs_helpers.h>
#include <util/translation.h>
#include <vault/crypter.h>
#include <vault/scriptpubkeyman.h>
#include <vault/transaction.h>
#include <vault/vault.h>
#include <vault/vaultdb.h>

#include <algorithm>
#include <cstdio>
#include <exception>

namespace vault {
namespace {

//! "QSVA" — Quicksilver vault archive.
constexpr uint32_t VAULT_ARCHIVE_MAGIC{0x41565351};
//! The container is versioned separately from the body: the container describes how the
//! bytes are protected, the body describes what they mean.
constexpr uint32_t VAULT_ARCHIVE_CONTAINER_VERSION{1};
//! Deliberately above the 25,000 used for vault key encryption. An archive is decrypted
//! once by hand, never on a hot path, so a slower derivation costs the user nothing.
constexpr unsigned int VAULT_ARCHIVE_DERIVE_ROUNDS{200'000};
//! Refuse absurd inputs rather than allocating from a hostile file.
constexpr uint64_t MAX_ARCHIVE_TRANSACTIONS{5'000'000};
constexpr uint64_t MAX_ARCHIVE_DESCRIPTORS{10'000};
constexpr uint64_t MAX_ARCHIVE_ADDRESSES{5'000'000};
//! The body is authenticated by prefixing its own hash before encryption; a wrong
//! passphrase then fails this check instead of producing garbage that happens to parse.
constexpr size_t ARCHIVE_CHECKSUM_SIZE{32};

void SerializeArchiveTx(DataStream& s, const VaultArchiveTx& tx)
{
    s << TX_WITH_WITNESS(tx.tx);
    s << tx.time_received << tx.time_smart << tx.from_me << tx.order_pos;
    s << tx.confirmed << tx.block_hash << tx.block_height << tx.block_index;
    s << tx.value_map;
}

void UnserializeArchiveTx(DataStream& s, VaultArchiveTx& tx)
{
    CMutableTransaction mtx;
    s >> TX_WITH_WITNESS(mtx);
    tx.tx = MakeTransactionRef(std::move(mtx));
    s >> tx.time_received >> tx.time_smart >> tx.from_me >> tx.order_pos;
    s >> tx.confirmed >> tx.block_hash >> tx.block_height >> tx.block_index;
    s >> tx.value_map;
}

void SerializeArchiveDescriptor(DataStream& s, const VaultArchiveDescriptor& desc)
{
    s << desc.descriptor << desc.creation_time << desc.active << desc.internal;
    s << desc.is_range << desc.range_start << desc.range_end << desc.next_index;
}

void UnserializeArchiveDescriptor(DataStream& s, VaultArchiveDescriptor& desc)
{
    s >> desc.descriptor >> desc.creation_time >> desc.active >> desc.internal;
    s >> desc.is_range >> desc.range_start >> desc.range_end >> desc.next_index;
}

void SerializeArchiveAddress(DataStream& s, const VaultArchiveAddress& addr)
{
    s << addr.destination << addr.has_label << addr.label << addr.purpose;
}

void UnserializeArchiveAddress(DataStream& s, VaultArchiveAddress& addr)
{
    s >> addr.destination >> addr.has_label >> addr.label >> addr.purpose;
}

DataStream SerializeArchiveBody(const VaultArchive& archive)
{
    DataStream body;
    body << archive.version << archive.chain << archive.genesis_hash << archive.vault_name;
    body << archive.birth_time << archive.created_time;
    body << static_cast<uint64_t>(archive.descriptors.size());
    for (const VaultArchiveDescriptor& desc : archive.descriptors) {
        SerializeArchiveDescriptor(body, desc);
    }
    body << static_cast<uint64_t>(archive.addresses.size());
    for (const VaultArchiveAddress& addr : archive.addresses) {
        SerializeArchiveAddress(body, addr);
    }
    body << static_cast<uint64_t>(archive.transactions.size());
    for (const VaultArchiveTx& tx : archive.transactions) {
        SerializeArchiveTx(body, tx);
    }
    return body;
}

util::Result<VaultArchive> UnserializeArchiveBody(DataStream& body)
{
    VaultArchive archive;
    body >> archive.version;
    if (archive.version == 0 || archive.version > VAULT_ARCHIVE_VERSION) {
        return util::Error{strprintf(_("Vault archive body version %u is not supported by this build."), archive.version)};
    }
    body >> archive.chain >> archive.genesis_hash >> archive.vault_name;
    body >> archive.birth_time >> archive.created_time;

    uint64_t descriptor_count{0};
    body >> descriptor_count;
    if (descriptor_count > MAX_ARCHIVE_DESCRIPTORS) {
        return util::Error{_("Vault archive declares more descriptors than this build will read.")};
    }
    archive.descriptors.resize(descriptor_count);
    for (VaultArchiveDescriptor& desc : archive.descriptors) {
        UnserializeArchiveDescriptor(body, desc);
    }

    uint64_t address_count{0};
    body >> address_count;
    if (address_count > MAX_ARCHIVE_ADDRESSES) {
        return util::Error{_("Vault archive declares more addresses than this build will read.")};
    }
    archive.addresses.resize(address_count);
    for (VaultArchiveAddress& addr : archive.addresses) {
        UnserializeArchiveAddress(body, addr);
    }

    uint64_t transaction_count{0};
    body >> transaction_count;
    if (transaction_count > MAX_ARCHIVE_TRANSACTIONS) {
        return util::Error{_("Vault archive declares more transactions than this build will read.")};
    }
    archive.transactions.resize(transaction_count);
    for (VaultArchiveTx& tx : archive.transactions) {
        UnserializeArchiveTx(body, tx);
    }
    return archive;
}

} // namespace

util::Result<VaultArchive> BuildVaultArchive(const CVault& vault)
{
    CHECK_NONFATAL(vault.IsVaultFlagSet(VAULT_FLAG_DESCRIPTORS));

    VaultArchive archive;
    archive.version = VAULT_ARCHIVE_VERSION;
    archive.chain = Params().GetChainTypeString();
    archive.genesis_hash = Params().GenesisBlock().GetHash();
    archive.vault_name = vault.GetName();
    archive.birth_time = vault.GetBirthTime();
    archive.created_time = GetTime();

    LOCK(vault.cs_vault);

    const auto active_spk_mans = vault.GetActiveScriptPubKeyMans();
    for (const auto& spk_man : vault.GetAllScriptPubKeyMans()) {
        const auto desc_spk_man = dynamic_cast<DescriptorScriptPubKeyMan*>(spk_man);
        if (!desc_spk_man) {
            return util::Error{_("Vault contains a script manager that cannot be archived.")};
        }
        LOCK(desc_spk_man->cs_desc_man);
        const auto& vault_descriptor = desc_spk_man->GetVaultDescriptor();

        VaultArchiveDescriptor desc;
        // Public form only. An archive is a record of history, not a second copy of the
        // keys — those are preserved by a vault-file backup.
        if (!desc_spk_man->GetDescriptorString(desc.descriptor, /*priv=*/false)) {
            return util::Error{_("Vault descriptor could not be written in its public form.")};
        }
        desc.creation_time = vault_descriptor.creation_time;
        desc.active = active_spk_mans.count(desc_spk_man) != 0;
        const auto internal = vault.IsInternalScriptPubKeyMan(desc_spk_man);
        desc.internal = internal.has_value() && *internal;
        desc.is_range = vault_descriptor.descriptor->IsRange();
        desc.range_start = vault_descriptor.range_start;
        desc.range_end = vault_descriptor.range_end;
        desc.next_index = vault_descriptor.next_index;
        archive.descriptors.push_back(std::move(desc));
    }

    // The address book is what tells a payment from change, so it is part of the history
    // rather than a nicety. See VaultArchiveAddress.
    for (const auto& [dest, data] : vault.m_address_book) {
        VaultArchiveAddress addr;
        addr.destination = EncodeDestination(dest);
        addr.has_label = data.label.has_value();
        if (addr.has_label) addr.label = *data.label;
        addr.purpose = data.purpose.has_value() ? static_cast<uint8_t>(*data.purpose) + 1 : 0;
        archive.addresses.push_back(std::move(addr));
    }
    std::sort(archive.addresses.begin(), archive.addresses.end(),
              [](const VaultArchiveAddress& a, const VaultArchiveAddress& b) {
                  return a.destination < b.destination;
              });

    archive.transactions.reserve(vault.mapVault.size());
    for (const auto& [txid, wtx] : vault.mapVault) {
        VaultArchiveTx tx;
        tx.tx = wtx.tx;
        tx.time_received = wtx.nTimeReceived;
        tx.time_smart = wtx.nTimeSmart;
        tx.from_me = wtx.fFromMe;
        tx.order_pos = wtx.nOrderPos;
        if (const auto* confirmed = std::get_if<TxStateConfirmed>(&wtx.m_state)) {
            tx.confirmed = true;
            tx.block_hash = confirmed->confirmed_block_hash;
            tx.block_height = confirmed->confirmed_block_height;
            tx.block_index = confirmed->position_in_block;
        } else if (std::holds_alternative<TxStateBlockUnresolved>(wtx.m_state)) {
            // The archive format records a confirmation height. Inventing one
            // would recreate the poisoned -1 state this path is designed to
            // eliminate, while dropping the block reference would silently
            // rewrite history as unconfirmed.
            return util::Error{_("Start consensus before exporting history from a vault with unresolved block confirmations.")};
        }
        for (const auto& [key, value] : wtx.mapValue) {
            tx.value_map.emplace_back(key, value);
        }
        archive.transactions.push_back(std::move(tx));
    }

    // Deterministic order, so two exports of an unchanged vault differ only in
    // created_time and the random salt. A user comparing archives should not have to
    // wonder whether a reordering means something changed.
    std::sort(archive.transactions.begin(), archive.transactions.end(),
              [](const VaultArchiveTx& a, const VaultArchiveTx& b) {
                  return a.tx->GetHash() < b.tx->GetHash();
              });

    return archive;
}

util::Result<void> WriteVaultArchive(const VaultArchive& archive,
                                     const fs::path& path,
                                     const SecureString& passphrase)
{
    if (passphrase.empty()) {
        return util::Error{_("A vault archive must be protected by a passphrase.")};
    }

    DataStream body{SerializeArchiveBody(archive)};

    // Prefix the body with its own hash before encrypting, so decryption with the wrong
    // passphrase is detected here rather than surfacing as a deserialization error.
    const uint256 body_hash{Hash(body)};
    CKeyingMaterial plaintext;
    plaintext.reserve(ARCHIVE_CHECKSUM_SIZE + body.size());
    plaintext.insert(plaintext.end(), body_hash.begin(), body_hash.end());
    for (const std::byte b : body) {
        plaintext.push_back(static_cast<unsigned char>(b));
    }

    std::vector<unsigned char> salt(VAULT_CRYPTO_SALT_SIZE);
    GetStrongRandBytes(salt);

    CCrypter crypter;
    if (!crypter.SetKeyFromPassphrase(passphrase, salt, VAULT_ARCHIVE_DERIVE_ROUNDS)) {
        return util::Error{_("Vault archive encryption key could not be derived.")};
    }
    std::vector<unsigned char> ciphertext;
    if (!crypter.Encrypt(plaintext, ciphertext)) {
        return util::Error{_("Vault archive could not be encrypted.")};
    }

    DataStream container;
    container << VAULT_ARCHIVE_MAGIC << VAULT_ARCHIVE_CONTAINER_VERSION;
    container << salt << static_cast<uint32_t>(VAULT_ARCHIVE_DERIVE_ROUNDS);
    container << ciphertext;

    // Write beside the target and rename, so an interrupted export cannot replace a good
    // archive with a truncated one. Losing history is the failure this feature exists to
    // prevent; it must not be the failure the feature introduces.
    const fs::path temp_path{fs::PathFromString(fs::PathToString(path) + ".new")};
    {
        AutoFile file{fsbridge::fopen(temp_path, "wb")};
        if (file.IsNull()) {
            return util::Error{strprintf(_("Vault archive %s could not be opened for writing."), fs::PathToString(temp_path))};
        }
        try {
            file << std::span{container};
        } catch (const std::exception&) {
            return util::Error{strprintf(_("Vault archive %s could not be written."), fs::PathToString(temp_path))};
        }
        if (!file.Commit()) {
            return util::Error{strprintf(_("Vault archive %s could not be committed to disk."), fs::PathToString(temp_path))};
        }
    }
    if (!RenameOver(temp_path, path)) {
        fs::remove_quiet(temp_path);
        return util::Error{strprintf(_("Vault archive could not be moved into place at %s."), fs::PathToString(path))};
    }
    return {};
}

util::Result<VaultArchive> ReadVaultArchive(const fs::path& path, const SecureString& passphrase)
{
    if (!fs::exists(path)) {
        return util::Error{strprintf(_("Vault archive %s does not exist."), fs::PathToString(path))};
    }

    std::vector<unsigned char> salt;
    std::vector<unsigned char> ciphertext;
    uint32_t rounds{0};
    {
        AutoFile file{fsbridge::fopen(path, "rb")};
        if (file.IsNull()) {
            return util::Error{strprintf(_("Vault archive %s could not be opened."), fs::PathToString(path))};
        }
        try {
            uint32_t magic{0};
            uint32_t container_version{0};
            file >> magic;
            if (magic != VAULT_ARCHIVE_MAGIC) {
                return util::Error{strprintf(_("%s is not a vault archive."), fs::PathToString(path))};
            }
            file >> container_version;
            if (container_version == 0 || container_version > VAULT_ARCHIVE_CONTAINER_VERSION) {
                return util::Error{strprintf(_("Vault archive version %u is not supported by this build."), container_version)};
            }
            file >> salt >> rounds >> ciphertext;
        } catch (const std::exception&) {
            return util::Error{strprintf(_("Vault archive %s is corrupt or truncated."), fs::PathToString(path))};
        }
    }
    if (salt.size() != VAULT_CRYPTO_SALT_SIZE || rounds == 0 || ciphertext.empty()) {
        return util::Error{strprintf(_("Vault archive %s is corrupt."), fs::PathToString(path))};
    }

    CCrypter crypter;
    if (!crypter.SetKeyFromPassphrase(passphrase, salt, rounds)) {
        return util::Error{_("Vault archive decryption key could not be derived.")};
    }
    CKeyingMaterial plaintext;
    if (!crypter.Decrypt(ciphertext, plaintext) || plaintext.size() < ARCHIVE_CHECKSUM_SIZE) {
        return util::Error{_("Vault archive passphrase is incorrect.")};
    }

    uint256 stored_hash;
    std::copy(plaintext.begin(), plaintext.begin() + ARCHIVE_CHECKSUM_SIZE, stored_hash.begin());
    DataStream body{std::span{plaintext}.subspan(ARCHIVE_CHECKSUM_SIZE)};
    if (Hash(body) != stored_hash) {
        // Padding makes a wrong passphrase usually fail in Decrypt above, but not always.
        return util::Error{_("Vault archive passphrase is incorrect.")};
    }

    try {
        return UnserializeArchiveBody(body);
    } catch (const std::exception&) {
        return util::Error{strprintf(_("Vault archive %s is corrupt."), fs::PathToString(path))};
    }
}

util::Result<VaultArchiveImportResult> ImportVaultArchive(CVault& vault, const VaultArchive& archive)
{
    CHECK_NONFATAL(vault.IsVaultFlagSet(VAULT_FLAG_DESCRIPTORS));
    if (archive.chain != Params().GetChainTypeString() ||
        archive.genesis_hash != Params().GenesisBlock().GetHash()) {
        return util::Error{strprintf(_("Vault archive was taken from the %s network and cannot be restored here."),
                                     archive.chain.empty() ? "unknown" : archive.chain)};
    }

    VaultArchiveImportResult result;

    LOCK(vault.cs_vault);

    for (const VaultArchiveDescriptor& archived : archive.descriptors) {
        FlatSigningProvider keys;
        std::string error;
        auto parsed = Parse(archived.descriptor, keys, error, /*require_checksum=*/false);
        if (parsed.empty()) {
            return util::Error{strprintf(_("Vault archive descriptor could not be parsed: %s"), error)};
        }

        VaultDescriptor desc(std::move(parsed.at(0)),
                             archived.creation_time,
                             static_cast<int32_t>(archived.range_start),
                             static_cast<int32_t>(archived.range_end),
                             static_cast<int32_t>(archived.next_index));

        if (vault.GetDescriptorScriptPubKeyMan(desc) != nullptr) continue;
        auto* spk_man = vault.AddVaultDescriptor(desc, keys, /*label=*/"", archived.internal);
        if (spk_man == nullptr) {
            return util::Error{_("Vault archive descriptor could not be added to this vault.")};
        }
        // Adding a descriptor does not activate it, and an inactive descriptor is treated
        // as change: every restored address would report ischange, and the restored vault
        // would hand out addresses from a fresh descriptor instead of the archived one.
        // Restore the active set the archive recorded.
        if (const auto output_type = desc.descriptor->GetOutputType()) {
            if (archived.active) {
                vault.AddActiveScriptPubKeyMan(spk_man->GetID(), *output_type, archived.internal);
            } else {
                vault.DeactivateScriptPubKeyMan(spk_man->GetID(), *output_type, archived.internal);
            }
        }
        ++result.descriptors_imported;
    }

    for (const VaultArchiveAddress& archived : archive.addresses) {
        const CTxDestination dest{DecodeDestination(archived.destination)};
        if (!IsValidDestination(dest)) {
            return util::Error{strprintf(_("Vault archive contains an address that is not valid on this network: %s"),
                                         archived.destination)};
        }
        std::optional<AddressPurpose> purpose;
        if (archived.purpose == 1) {
            purpose = AddressPurpose::RECEIVE;
        } else if (archived.purpose == 2) {
            purpose = AddressPurpose::SEND;
        }
        // Restore the label EXACTLY as recorded, including a label that is present but
        // empty: "" means a real address with the default label, while no label at all
        // means change, and collapsing the two would rewrite the user's history.
        if (!vault.SetAddressBook(dest, archived.has_label ? archived.label : std::string{}, purpose)) {
            return util::Error{strprintf(_("Vault archive address %s could not be restored."), archived.destination)};
        }
        ++result.addresses_imported;
    }

    for (const VaultArchiveTx& archived : archive.transactions) {
        if (archived.tx == nullptr) continue;
        if (vault.mapVault.count(archived.tx->GetHash()) != 0) {
            ++result.transactions_skipped;
            continue;
        }
        const TxState state = archived.confirmed
            ? TxState{TxStateConfirmed{archived.block_hash, archived.block_height, archived.block_index}}
            : TxState{TxStateInactive{}};
        CVaultTx* wtx = vault.AddToVault(archived.tx, state, [&](CVaultTx& new_tx, bool /*new_entry*/) {
            new_tx.nTimeReceived = archived.time_received;
            new_tx.nTimeSmart = archived.time_smart;
            new_tx.fFromMe = archived.from_me;
            new_tx.nOrderPos = archived.order_pos;
            for (const auto& [key, value] : archived.value_map) {
                new_tx.mapValue[key] = value;
            }
            return true;
        });
        if (wtx == nullptr) {
            return util::Error{_("Vault archive transaction could not be restored into this vault.")};
        }
        ++result.transactions_imported;
    }

    if (archive.birth_time > 0) vault.MaybeUpdateBirthTime(archive.birth_time);

    return result;
}

} // namespace vault
