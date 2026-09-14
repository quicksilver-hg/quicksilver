// Copyright (c) 2017-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_VAULT_VAULTUTIL_H
#define QUICKSILVER_VAULT_VAULTUTIL_H

#include <script/descriptor.h>
#include <util/fs.h>

#include <vector>

namespace vault {
//! Vault files with a version greater than Quicksilver 0.1.0 are too new to open.
static constexpr int VAULT_FILE_VERSION{100};

enum VaultFlags : uint64_t {
    // vault flags in the upper section (> 1 << 31) will lead to not opening the vault if flag is unknown
    // unknown vault flags in the lower section <= (1 << 31) will be tolerated

    // will categorize coins as clean (not reused) and dirty (reused), and handle
    // them with privacy considerations in mind
    VAULT_FLAG_AVOID_REUSE = (1ULL << 0),

    // will enforce the rule that the vault can't contain any private keys (public keys only)
    VAULT_FLAG_DISABLE_PRIVATE_KEYS = (1ULL << 32),

    //! Flag set when a vault contains no HD seed and no private keys, scripts,
    //! or addresses, and is therefore "blank."
    //!
    //! The main function this flag serves is to distinguish a blank vault from
    //! a newly created vault when the vault database is loaded, to avoid
    //! initialization that should only happen on first run.
    //!
    //! A secondary function of this flag is to serve as an ongoing indication
    //! that descriptors in the vault should be created manually, and that the
    //! vault should not automatically generate new descriptors if it is later
    //! encrypted. Vaults do not automatically unset the BLANK flag
    //! when things are imported.
    //!
    //! This flag is also a mandatory flag to prevent previous versions of
    //! Quicksilver from opening the vault, thinking it was newly created, and
    //! then improperly reinitializing it.
    VAULT_FLAG_BLANK_VAULT = (1ULL << 33),

    //! Indicate that this vault supports DescriptorScriptPubKeyMan
    VAULT_FLAG_DESCRIPTORS = (1ULL << 34),

    //! Indicates that the vault needs an external signer
    VAULT_FLAG_EXTERNAL_SIGNER = (1ULL << 35),
};

//! Get the path of the vault directory.
fs::path GetVaultDir();

/** Descriptor with some vault metadata */
class VaultDescriptor
{
public:
    std::shared_ptr<Descriptor> descriptor;
    uint256 id; // Descriptor ID (calculated once at descriptor initialization/deserialization)
    uint64_t creation_time = 0;
    int32_t range_start = 0; // First item in range; start of range, inclusive, i.e. [range_start, range_end). This never changes.
    int32_t range_end = 0; // Item after the last; end of range, exclusive, i.e. [range_start, range_end). This will increment with each TopUp()
    int32_t next_index = 0; // Position of the next item to generate
    DescriptorCache cache;

    void DeserializeDescriptor(const std::string& str)
    {
        std::string error;
        FlatSigningProvider keys;
        auto descs = Parse(str, keys, error, true);
        if (descs.empty()) {
            throw std::ios_base::failure("Invalid descriptor: " + error);
        }
        if (descs.size() > 1) {
            throw std::ios_base::failure("Can't load a multipath descriptor from databases");
        }
        descriptor = std::move(descs.at(0));
        id = DescriptorID(*descriptor);
    }

    SERIALIZE_METHODS(VaultDescriptor, obj)
    {
        std::string descriptor_str;
        SER_WRITE(obj, descriptor_str = obj.descriptor->ToString());
        READWRITE(descriptor_str, obj.creation_time, obj.next_index, obj.range_start, obj.range_end);
        SER_READ(obj, obj.DeserializeDescriptor(descriptor_str));
    }

    VaultDescriptor() = default;
    VaultDescriptor(std::shared_ptr<Descriptor> descriptor, uint64_t creation_time, int32_t range_start, int32_t range_end, int32_t next_index) : descriptor(descriptor), id(DescriptorID(*descriptor)), creation_time(creation_time), range_start(range_start), range_end(range_end), next_index(next_index) { }
};

VaultDescriptor GenerateVaultDescriptor(const CExtPubKey& master_key, const OutputType& output_type, bool internal);
} // namespace vault

#endif // QUICKSILVER_VAULT_VAULTUTIL_H
