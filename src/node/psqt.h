// Copyright (c) 2009-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_NODE_PSQT_H
#define QUICKSILVER_NODE_PSQT_H

#include <psqt.h>

#include <optional>

namespace node {
/**
 * Holds an analysis of one input from a PSQT
 */
struct PSQTInputAnalysis {
    bool has_utxo; //!< Whether we have UTXO information for this input
    bool is_final; //!< Whether the input has all required information including signatures
    PSQTRole next; //!< Which of the BIP 174 roles needs to handle this input next

    std::vector<CKeyID> missing_pubkeys; //!< Pubkeys whose BIP32 derivation path is missing
    std::vector<CKeyID> missing_sigs;    //!< Pubkeys whose signatures are missing
    uint160 missing_redeem_script;       //!< Hash160 of redeem script, if missing
    uint256 missing_witness_script;      //!< SHA256 of witness script, if missing
};

/**
 * Holds the results of AnalyzePSQT (miscellaneous information about a PSQT)
 */
struct PSQTAnalysis {
    std::optional<size_t> estimated_vsize; //!< Estimated weight of the transaction
    std::optional<CAmount> value_delta;    //!< Difference between input and output amounts
    std::vector<PSQTInputAnalysis> inputs; //!< More information about the individual inputs of the transaction
    PSQTRole next;                         //!< Which of the BIP 174 roles needs to handle the transaction next
    std::string error;                     //!< Error message

    void SetInvalid(std::string err_msg)
    {
        estimated_vsize = std::nullopt;
        value_delta = std::nullopt;
        inputs.clear();
        next = PSQTRole::CREATOR;
        error = err_msg;
    }
};

/**
 * Provides helpful miscellaneous information about where a PSQT is in the signing workflow.
 *
 * @param[in] psqtx the PSQT to analyze
 * @return A PSQTAnalysis with information about the provided PSQT.
 */
PSQTAnalysis AnalyzePSQT(PartiallySignedQuicksilverTransaction psqtx);
} // namespace node

#endif // QUICKSILVER_NODE_PSQT_H
