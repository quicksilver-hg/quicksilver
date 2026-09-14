// Copyright (c) 2018-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_EXTERNAL_SIGNER_H
#define QUICKSILVER_EXTERNAL_SIGNER_H

#include <common/system.h>
#include <univalue.h>

#include <string>
#include <vector>

struct PartiallySignedQuicksilverTransaction;

//! Enables interaction with an external signing device or service, such as
//! a hardware vault. See doc/external-signer.md
class ExternalSigner
{
private:
    //! The command which handles interaction with the external signer.
    std::string m_command;

    //! Quicksilver chain name.
    std::string m_chain;

    std::string NetworkArg() const;

public:
    //! @param[in] command      the command which handles interaction with the external signer
    //! @param[in] fingerprint  master key fingerprint of the signer
    //! @param[in] chain        "main", "publictest" or "sandbox"
    //! @param[in] name         device name
    ExternalSigner(const std::string& command, const std::string chain, const std::string& fingerprint, const std::string name);

    //! Master key fingerprint of the signer
    std::string m_fingerprint;

    //! Name of signer
    std::string m_name;

    //! Obtain a list of signers. Calls `<command> enumerate`.
    //! @param[in]              command the command which handles interaction with the external signer
    //! @param[in,out] signers  vector to which new signers (with a unique master key fingerprint) are added
    //! @param chain            "main", "publictest" or "sandbox"
    //! @returns success
    static bool Enumerate(const std::string& command, std::vector<ExternalSigner>& signers, const std::string chain);

    //! Display address on the device. Calls `<command> displayaddress --desc <descriptor>`.
    //! @param[in] descriptor Descriptor specifying which address to display.
    //!            Must include a public key or qpub, as well as key origin.
    UniValue DisplayAddress(const std::string& descriptor) const;

    //! Get receive and change Descriptor(s) from device for a given account.
    //! Calls `<command> getdescriptors --account <account>`
    //! @param[in] account  which BIP32 account to use (e.g. `m/44'/<coin>'/account'`)
    //! @returns see doc/external-signer.md
    UniValue GetDescriptors(const int account);

    //! Sign PartiallySignedQuicksilverTransaction on the device.
    //! Calls `<command> signtx` and passes the PSQT via stdin.
    //! @param[in,out] psqt  PartiallySignedQuicksilverTransaction to be signed
    bool SignTransaction(PartiallySignedQuicksilverTransaction& psqt, std::string& error);
};

#endif // QUICKSILVER_EXTERNAL_SIGNER_H
