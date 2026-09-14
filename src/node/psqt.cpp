// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>
#include <consensus/amount.h>
#include <consensus/tx_verify.h>
#include <node/psqt.h>
#include <policy/policy.h>
#include <policy/settings.h>
#include <tinyformat.h>

#include <numeric>

namespace node {
PSQTAnalysis AnalyzePSQT(PartiallySignedQuicksilverTransaction psqtx)
{
    // Go through each input and build status
    PSQTAnalysis result;

    bool calc_value_delta = true;

    CAmount in_amt = 0;

    result.inputs.resize(psqtx.tx->vin.size());

    const PrecomputedTransactionData txdata = PrecomputePSQTData(psqtx);

    for (unsigned int i = 0; i < psqtx.tx->vin.size(); ++i) {
        PSQTInput& input = psqtx.inputs[i];
        PSQTInputAnalysis& input_analysis = result.inputs[i];

        // We set next role here and ratchet backwards as required
        input_analysis.next = PSQTRole::EXTRACTOR;

        // Check for a UTXO
        CTxOut utxo;
        if (psqtx.GetInputUTXO(utxo, i)) {
            if (!MoneyRange(utxo.nValue) || !MoneyRange(in_amt + utxo.nValue)) {
                result.SetInvalid(strprintf("PSQT is not valid. Input %u has invalid value", i));
                return result;
            }
            in_amt += utxo.nValue;
            input_analysis.has_utxo = true;
        } else {
            if (input.non_witness_utxo && psqtx.tx->vin[i].prevout.n >= input.non_witness_utxo->vout.size()) {
                result.SetInvalid(strprintf("PSQT is not valid. Input %u specifies invalid prevout", i));
                return result;
            }
            input_analysis.has_utxo = false;
            input_analysis.is_final = false;
            input_analysis.next = PSQTRole::UPDATER;
            calc_value_delta = false;
        }

        if (!utxo.IsNull() && utxo.scriptPubKey.IsUnspendable()) {
            result.SetInvalid(strprintf("PSQT is not valid. Input %u spends unspendable output", i));
            return result;
        }

        // Check if it is final
        if (!PSQTInputSignedAndVerified(psqtx, i, &txdata)) {
            input_analysis.is_final = false;

            // Figure out what is missing
            SignatureData outdata;
            bool complete = SignPSQTInput(DUMMY_SIGNING_PROVIDER, psqtx, i, &txdata, 1, &outdata);

            // Things are missing
            if (!complete) {
                input_analysis.missing_pubkeys = outdata.missing_pubkeys;
                input_analysis.missing_redeem_script = outdata.missing_redeem_script;
                input_analysis.missing_witness_script = outdata.missing_witness_script;
                input_analysis.missing_sigs = outdata.missing_sigs;

                // If we are only missing signatures and nothing else, then next is signer
                if (outdata.missing_pubkeys.empty() && outdata.missing_redeem_script.IsNull() && outdata.missing_witness_script.IsNull() && !outdata.missing_sigs.empty()) {
                    input_analysis.next = PSQTRole::SIGNER;
                } else {
                    input_analysis.next = PSQTRole::UPDATER;
                }
            } else {
                input_analysis.next = PSQTRole::FINALIZER;
            }
        } else if (!utxo.IsNull()){
            input_analysis.is_final = true;
        }
    }

    // Calculate next role for PSQT by grabbing "minimum" PSQTInput next role
    result.next = PSQTRole::EXTRACTOR;
    for (unsigned int i = 0; i < psqtx.tx->vin.size(); ++i) {
        PSQTInputAnalysis& input_analysis = result.inputs[i];
        result.next = std::min(result.next, input_analysis.next);
    }
    assert(result.next > PSQTRole::CREATOR);

    if (calc_value_delta) {
        // Get the output amount
        CAmount out_amt = std::accumulate(psqtx.tx->vout.begin(), psqtx.tx->vout.end(), CAmount(0),
            [](CAmount a, const CTxOut& b) {
                if (!MoneyRange(a) || !MoneyRange(b.nValue) || !MoneyRange(a + b.nValue)) {
                    return CAmount(-1);
                }
                return a += b.nValue;
            }
        );
        if (!MoneyRange(out_amt)) {
            result.SetInvalid("PSQT is not valid. Output amount invalid");
            return result;
        }

        // Compare inputs and outputs; Quicksilver PSQTs must be exact-value.
        CAmount value_delta = in_amt - out_amt;
        result.value_delta = value_delta;

        // Estimate the size
        CMutableTransaction mtx(*psqtx.tx);
        CCoinsView view_dummy;
        CCoinsViewCache view(&view_dummy);
        bool success = true;

        for (unsigned int i = 0; i < psqtx.tx->vin.size(); ++i) {
            PSQTInput& input = psqtx.inputs[i];
            Coin newcoin;

            if (!SignPSQTInput(DUMMY_SIGNING_PROVIDER, psqtx, i, nullptr, 1) || !psqtx.GetInputUTXO(newcoin.out, i)) {
                success = false;
                break;
            } else {
                mtx.vin[i].scriptSig = input.final_script_sig;
                mtx.vin[i].scriptWitness = input.final_script_witness;
                newcoin.nHeight = 1;
                view.AddCoin(psqtx.tx->vin[i].prevout, std::move(newcoin), true);
            }
        }

        if (success) {
            CTransaction ctx = CTransaction(mtx);
            size_t size(GetVirtualTransactionSize(ctx, GetTransactionSigOpCost(ctx, view, STANDARD_SCRIPT_VERIFY_FLAGS), ::nBytesPerSigOp));
            result.estimated_vsize = size;
        }

    }

    return result;
}
} // namespace node
