// Copyright (c) 2017-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <vault/vaultutil.h>

#include <chainparams.h>
#include <common/args.h>
#include <key_io.h>
#include <logging.h>
#include <tinyformat.h>

namespace vault {
fs::path GetVaultDir()
{
    fs::path path;

    if (gArgs.IsArgSet("-vaultdir")) {
        path = gArgs.GetPathArg("-vaultdir");
        if (!fs::is_directory(path)) {
            // If the path specified doesn't exist, we return the deliberately
            // invalid empty string.
            path = "";
        }
    } else {
        path = gArgs.GetDataDirNet() / "vaults";
    }

    return path;
}

VaultDescriptor GenerateVaultDescriptor(const CExtPubKey& master_key, const OutputType& addr_type, bool internal)
{
    int64_t creation_time = GetTime();

    std::string xpub = EncodeExtPubKey(master_key);

    // Build descriptor string
    std::string desc_prefix;
    std::string desc_suffix = "/*)";
    switch (addr_type) {
    case OutputType::BASE58: {
        desc_prefix = "pkh(" + xpub + "/44h";
        break;
    }
    case OutputType::BECH32: {
        desc_prefix = "wpkh(" + xpub + "/84h";
        break;
    }
    case OutputType::BECH32M: {
        desc_prefix = "tr(" + xpub + "/86h";
        break;
    }
    case OutputType::UNKNOWN: {
        // We should never have a DescriptorScriptPubKeyMan for an UNKNOWN OutputType,
        // so if we get to this point something is wrong
        assert(false);
    }
    } // no default case, so the compiler can warn about missing cases
    assert(!desc_prefix.empty());

    desc_prefix += strprintf("/%uh", Params().Slip44CoinType());

    std::string internal_path = internal ? "/1" : "/0";
    std::string desc_str = desc_prefix + "/0h" + internal_path + desc_suffix;

    // Make the descriptor
    FlatSigningProvider keys;
    std::string error;
    std::vector<std::unique_ptr<Descriptor>> desc = Parse(desc_str, keys, error, false);
    VaultDescriptor w_desc(std::move(desc.at(0)), creation_time, 0, 0, 0);
    return w_desc;
}

} // namespace vault
