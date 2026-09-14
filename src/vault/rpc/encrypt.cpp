// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <rpc/util.h>
#include <vault/rpc/util.h>
#include <vault/vault.h>


namespace vault {
RPCHelpMan vaultpassphrase()
{
    return RPCHelpMan{"vaultpassphrase",
                "\nStores the vault decryption key in memory for 'timeout' seconds.\n"
                "This is needed prior to performing transactions related to private keys such as sending coins\n"
            "\nNote:\n"
            "Issuing the vaultpassphrase command while the vault is already unlocked will set a new unlock\n"
            "time that overrides the old one.\n",
                {
                    {"passphrase", RPCArg::Type::STR, RPCArg::Optional::NO, "The vault passphrase"},
                    {"timeout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The time to keep the decryption key in seconds; capped at 100000000 (~3 years)."},
                },
                RPCResult{RPCResult::Type::NONE, "", ""},
                RPCExamples{
            "\nUnlock the vault for 60 seconds\n"
            + HelpExampleCli("vaultpassphrase", "\"my pass phrase\" 60") +
            "\nLock the vault again (before 60 seconds)\n"
            + HelpExampleCli("vaultlock", "") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("vaultpassphrase", "\"my pass phrase\", 60")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CVault> const vault = GetVaultForJSONRPCRequest(request);
    if (!vault) return UniValue::VNULL;
    CVault* const pvault = vault.get();

    int64_t nSleepTime;
    int64_t relock_time;
    // Prevent concurrent calls to vaultpassphrase with the same vault.
    LOCK(pvault->m_unlock_mutex);
    {
        LOCK(pvault->cs_vault);

        if (!pvault->IsCrypted()) {
            throw JSONRPCError(RPC_VAULT_WRONG_ENC_STATE, "Error: running with an unencrypted vault, but vaultpassphrase was called.");
        }

        // Note that the vaultpassphrase is stored in request.params[0] which is not mlock()ed
        SecureString strVaultPass;
        strVaultPass.reserve(100);
        strVaultPass = std::string_view{request.params[0].get_str()};

        // Get the timeout
        nSleepTime = request.params[1].getInt<int64_t>();
        // Timeout cannot be negative, otherwise it will relock immediately
        if (nSleepTime < 0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Timeout cannot be negative.");
        }
        // Clamp timeout
        constexpr int64_t MAX_SLEEP_TIME = 100000000; // larger values trigger a macos/libevent bug?
        if (nSleepTime > MAX_SLEEP_TIME) {
            nSleepTime = MAX_SLEEP_TIME;
        }

        if (strVaultPass.empty()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "passphrase cannot be empty");
        }

        if (!pvault->Unlock(strVaultPass)) {
            throw JSONRPCError(RPC_VAULT_PASSPHRASE_INCORRECT, "Error: The vault passphrase entered was incorrect.");
        }

        pvault->TopUpKeyPool();

        pvault->nRelockTime = GetTime() + nSleepTime;
        relock_time = pvault->nRelockTime;
    }

    // rpcRunLater must be called without cs_vault held otherwise a deadlock
    // can occur. The deadlock would happen when RPCRunLater removes the
    // previous timer (and waits for the callback to finish if already running)
    // and the callback locks cs_vault.
    AssertLockNotHeld(vault->cs_vault);
    // Keep a weak pointer to the vault so that it is possible to unload the
    // vault before the following callback is called. If a valid shared pointer
    // is acquired in the callback then the vault is still loaded.
    std::weak_ptr<CVault> weak_vault = vault;
    pvault->chain().rpcRunLater(strprintf("lockvault(%s)", pvault->GetName()), [weak_vault, relock_time] {
        if (auto shared_vault = weak_vault.lock()) {
            LOCK2(shared_vault->m_relock_mutex, shared_vault->cs_vault);
            // Skip if this is not the most recent rpcRunLater callback.
            if (shared_vault->nRelockTime != relock_time) return;
            shared_vault->Lock();
            shared_vault->nRelockTime = 0;
        }
    }, nSleepTime);

    return UniValue::VNULL;
},
    };
}


RPCHelpMan vaultpassphrasechange()
{
    return RPCHelpMan{"vaultpassphrasechange",
                "\nChanges the vault passphrase from 'oldpassphrase' to 'newpassphrase'.\n",
                {
                    {"oldpassphrase", RPCArg::Type::STR, RPCArg::Optional::NO, "The current passphrase"},
                    {"newpassphrase", RPCArg::Type::STR, RPCArg::Optional::NO, "The new passphrase"},
                },
                RPCResult{RPCResult::Type::NONE, "", ""},
                RPCExamples{
                    HelpExampleCli("vaultpassphrasechange", "\"old one\" \"new one\"")
            + HelpExampleRpc("vaultpassphrasechange", "\"old one\", \"new one\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CVault> const pvault = GetVaultForJSONRPCRequest(request);
    if (!pvault) return UniValue::VNULL;

    if (!pvault->IsCrypted()) {
        throw JSONRPCError(RPC_VAULT_WRONG_ENC_STATE, "Error: running with an unencrypted vault, but vaultpassphrasechange was called.");
    }

    if (pvault->IsScanningWithPassphrase()) {
        throw JSONRPCError(RPC_VAULT_ERROR, "Error: the vault is currently being used to rescan the blockchain for related transactions. Please call `abortrescan` before changing the passphrase.");
    }

    LOCK2(pvault->m_relock_mutex, pvault->cs_vault);

    SecureString strOldVaultPass;
    strOldVaultPass.reserve(100);
    strOldVaultPass = std::string_view{request.params[0].get_str()};

    SecureString strNewVaultPass;
    strNewVaultPass.reserve(100);
    strNewVaultPass = std::string_view{request.params[1].get_str()};

    if (strOldVaultPass.empty() || strNewVaultPass.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "passphrase cannot be empty");
    }

    if (!pvault->ChangeVaultPassphrase(strOldVaultPass, strNewVaultPass)) {
        throw JSONRPCError(RPC_VAULT_PASSPHRASE_INCORRECT, "Error: The vault passphrase entered was incorrect.");
    }

    return UniValue::VNULL;
},
    };
}


RPCHelpMan vaultlock()
{
    return RPCHelpMan{"vaultlock",
                "\nRemoves the vault encryption key from memory, locking the vault.\n"
                "After calling this method, you will need to call vaultpassphrase again\n"
                "before being able to call any methods which require the vault to be unlocked.\n",
                {},
                RPCResult{RPCResult::Type::NONE, "", ""},
                RPCExamples{
            "\nSet the passphrase for 2 minutes to perform a transaction\n"
            + HelpExampleCli("vaultpassphrase", "\"my pass phrase\" 120") +
            "\nPerform a send (requires passphrase set)\n"
            + HelpExampleCli("sendtoaddress", "\"" + EXAMPLE_ADDRESS[0] + "\" 1.0") +
            "\nClear the passphrase since we are done before 2 minutes is up\n"
            + HelpExampleCli("vaultlock", "") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("vaultlock", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CVault> const pvault = GetVaultForJSONRPCRequest(request);
    if (!pvault) return UniValue::VNULL;

    if (!pvault->IsCrypted()) {
        throw JSONRPCError(RPC_VAULT_WRONG_ENC_STATE, "Error: running with an unencrypted vault, but vaultlock was called.");
    }

    if (pvault->IsScanningWithPassphrase()) {
        throw JSONRPCError(RPC_VAULT_ERROR, "Error: the vault is currently being used to rescan the blockchain for related transactions. Please call `abortrescan` before locking the vault.");
    }

    LOCK2(pvault->m_relock_mutex, pvault->cs_vault);

    pvault->Lock();
    pvault->nRelockTime = 0;

    return UniValue::VNULL;
},
    };
}


RPCHelpMan encryptvault()
{
    return RPCHelpMan{"encryptvault",
                "\nEncrypts the vault with 'passphrase'. This is for first time encryption.\n"
                "After this, any calls that interact with private keys such as sending or signing \n"
                "will require the passphrase to be set prior the making these calls.\n"
                "Use the vaultpassphrase call for this, and then vaultlock call.\n"
                "If the vault is already encrypted, use the vaultpassphrasechange call.\n"
                "** IMPORTANT **\n"
                "For security reasons, the encryption process will generate a new HD seed, resulting\n"
                "in the creation of a fresh set of active descriptors. Therefore, it is crucial to\n"
                "securely back up the newly generated vault file using the backupvault RPC.\n",
                {
                    {"passphrase", RPCArg::Type::STR, RPCArg::Optional::NO, "The pass phrase to encrypt the vault with. It must be at least 1 character, but should be long."},
                },
                RPCResult{RPCResult::Type::STR, "", "A string with further instructions"},
                RPCExamples{
            "\nEncrypt your vault\n"
            + HelpExampleCli("encryptvault", "\"my pass phrase\"") +
            "\nNow set the passphrase to use the vault, such as for signing or sending coins\n"
            + HelpExampleCli("vaultpassphrase", "\"my pass phrase\"") +
            "\nNow we can do something like sign\n"
            + HelpExampleCli("signmessage", "\"address\" \"test message\"") +
            "\nNow lock the vault again by removing the passphrase\n"
            + HelpExampleCli("vaultlock", "") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("encryptvault", "\"my pass phrase\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CVault> const pvault = GetVaultForJSONRPCRequest(request);
    if (!pvault) return UniValue::VNULL;

    if (pvault->IsVaultFlagSet(VAULT_FLAG_DISABLE_PRIVATE_KEYS)) {
        throw JSONRPCError(RPC_VAULT_ENCRYPTION_FAILED, "Error: vault does not contain private keys, nothing to encrypt.");
    }

    if (pvault->IsCrypted()) {
        throw JSONRPCError(RPC_VAULT_WRONG_ENC_STATE, "Error: running with an encrypted vault, but encryptvault was called.");
    }

    if (pvault->IsScanningWithPassphrase()) {
        throw JSONRPCError(RPC_VAULT_ERROR, "Error: the vault is currently being used to rescan the blockchain for related transactions. Please call `abortrescan` before encrypting the vault.");
    }

    LOCK2(pvault->m_relock_mutex, pvault->cs_vault);

    SecureString strVaultPass;
    strVaultPass.reserve(100);
    strVaultPass = std::string_view{request.params[0].get_str()};

    if (strVaultPass.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "passphrase cannot be empty");
    }

    if (!pvault->EncryptVault(strVaultPass)) {
        throw JSONRPCError(RPC_VAULT_ENCRYPTION_FAILED, "Error: Failed to encrypt the vault.");
    }

    return "vault encrypted; The keypool has been flushed and a new HD seed was generated. You need to make a new backup with the backupvault RPC.";
},
    };
}
} // namespace vault
