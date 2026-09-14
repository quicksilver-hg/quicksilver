// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <common/messages.h>

#include <common/types.h>
#include <node/types.h>
#include <tinyformat.h>
#include <util/string.h>
#include <util/translation.h>

#include <cassert>
#include <string>

using node::TransactionError;

namespace common {
bilingual_str PSQTErrorString(PSQTError err)
{
    switch (err) {
        case PSQTError::MISSING_INPUTS:
            return Untranslated("Inputs missing or spent");
        case PSQTError::SIGHASH_MISMATCH:
            return Untranslated("Specified sighash value does not match value stored in PSQT");
        case PSQTError::EXTERNAL_SIGNER_NOT_FOUND:
            return Untranslated("External signer not found");
        case PSQTError::EXTERNAL_SIGNER_FAILED:
            return Untranslated("External signer failed to sign");
        case PSQTError::UNSUPPORTED:
            return Untranslated("Signer does not support PSQT");
        // no default case, so the compiler can warn about missing cases
    }
    assert(false);
}

bilingual_str TransactionErrorString(const TransactionError err)
{
    switch (err) {
        case TransactionError::OK:
            return Untranslated("No error");
        case TransactionError::MISSING_INPUTS:
            return Untranslated("Inputs missing or spent");
        case TransactionError::ALREADY_IN_UTXO_SET:
            return Untranslated("Transaction outputs already in utxo set");
        case TransactionError::RELAYPOOL_REJECTED:
            return Untranslated("Transaction rejected by relay pool");
        case TransactionError::CONSENSUS_INVALID:
            return Untranslated("Transaction is invalid by consensus rules");
        case TransactionError::RELAYPOOL_ERROR:
            return Untranslated("Relay pool internal error");
        case TransactionError::MAX_BURN_EXCEEDED:
            return Untranslated("Unspendable output exceeds maximum configured by user (maxburnamount)");
        case TransactionError::INVALID_PACKAGE:
            return Untranslated("Transaction rejected due to invalid package");
        // no default case, so the compiler can warn about missing cases
    }
    assert(false);
}

bilingual_str ResolveErrMsg(const std::string& optname, const std::string& strBind)
{
    return strprintf(_("Cannot resolve -%s address: '%s'"), optname, strBind);
}

bilingual_str InvalidPortErrMsg(const std::string& optname, const std::string& invalid_value)
{
    return strprintf(_("Invalid port specified in %s: '%s'"), optname, invalid_value);
}

bilingual_str AmountHighWarn(const std::string& optname)
{
    return strprintf(_("%s is set very high!"), optname);
}

bilingual_str AmountErrMsg(const std::string& optname, const std::string& strValue)
{
    return strprintf(_("Invalid amount for -%s=<amount>: '%s'"), optname, strValue);
}
} // namespace common
