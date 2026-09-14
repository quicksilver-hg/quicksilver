// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <common/signmessage.h>
#include <key_io.h>
#include <rpc/util.h>
#include <vault/rpc/util.h>
#include <vault/vault.h>

#include <univalue.h>

namespace vault {
RPCHelpMan signmessage()
{
    return RPCHelpMan{"signmessage",
        "\nSign a message with the private key of an address" +
          HELP_REQUIRING_PASSPHRASE,
        {
            {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The Quicksilver address to use for the private key."},
            {"message", RPCArg::Type::STR, RPCArg::Optional::NO, "The message to create a signature of."},
        },
        RPCResult{
            RPCResult::Type::STR, "signature", "The signature of the message encoded in base 64"
        },
        RPCExamples{
            "\nUnlock the vault for 30 seconds\n"
            + HelpExampleCli("vaultpassphrase", "\"mypassphrase\" 30") +
            "\nCreate the signature\n"
            + HelpExampleCli("signmessage", "\"QbURpsSa6XFTg61KScF1yntyVDVb1742e2\" \"my message\"") +
            "\nVerify the signature\n"
            + HelpExampleCli("verifymessage", "\"QbURpsSa6XFTg61KScF1yntyVDVb1742e2\" \"signature\" \"my message\"") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("signmessage", "\"QbURpsSa6XFTg61KScF1yntyVDVb1742e2\", \"my message\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            const std::shared_ptr<const CVault> pvault = GetVaultForJSONRPCRequest(request);
            if (!pvault) return UniValue::VNULL;

            LOCK(pvault->cs_vault);

            EnsureVaultIsUnlocked(*pvault);

            std::string strAddress = request.params[0].get_str();
            std::string strMessage = request.params[1].get_str();

            CTxDestination dest = DecodeDestination(strAddress);
            if (!IsValidDestination(dest)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
            }

            const PKHash* pkhash = std::get_if<PKHash>(&dest);
            if (!pkhash) {
                throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");
            }

            std::string signature;
            SigningResult err = pvault->SignMessage(strMessage, *pkhash, signature);
            if (err == SigningResult::SIGNING_FAILED) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, SigningResultString(err));
            } else if (err != SigningResult::OK) {
                throw JSONRPCError(RPC_VAULT_ERROR, SigningResultString(err));
            }

            return signature;
        },
    };
}
} // namespace vault
