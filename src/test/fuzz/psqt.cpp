// Copyright (c) 2019-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/psqt.h>
#include <psqt.h>
#include <pubkey.h>
#include <script/script.h>
#include <streams.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/util/random.h>
#include <util/check.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using node::AnalyzePSQT;
using node::PSQTAnalysis;
using node::PSQTInputAnalysis;

FUZZ_TARGET(psqt)
{
    SeedRandomStateForTest(SeedRand::ZEROS);
    FuzzedDataProvider fuzzed_data_provider{buffer.data(), buffer.size()};
    PartiallySignedQuicksilverTransaction psqt_mut;
    std::string error;
    auto str = fuzzed_data_provider.ConsumeRandomLengthString();
    if (!DecodeRawPSQT(psqt_mut, MakeByteSpan(str), error)) {
        return;
    }
    const PartiallySignedQuicksilverTransaction psqt = psqt_mut;

    const PSQTAnalysis analysis = AnalyzePSQT(psqt);
    (void)PSQTRoleName(analysis.next);
    for (const PSQTInputAnalysis& input_analysis : analysis.inputs) {
        (void)PSQTRoleName(input_analysis.next);
    }

    (void)psqt.IsNull();

    std::optional<CMutableTransaction> tx = psqt.tx;
    if (tx) {
        const CMutableTransaction& mtx = *tx;
        const PartiallySignedQuicksilverTransaction psqt_from_tx{mtx};
    }

    for (const PSQTInput& input : psqt.inputs) {
        (void)PSQTInputSigned(input);
        (void)input.IsNull();
    }
    (void)CountPSQTUnsignedInputs(psqt);

    for (const PSQTOutput& output : psqt.outputs) {
        (void)output.IsNull();
    }

    for (size_t i = 0; i < psqt.tx->vin.size(); ++i) {
        CTxOut tx_out;
        if (psqt.GetInputUTXO(tx_out, i)) {
            (void)tx_out.IsNull();
            (void)tx_out.ToString();
        }
    }

    psqt_mut = psqt;
    (void)FinalizePSQT(psqt_mut);

    psqt_mut = psqt;
    CMutableTransaction result;
    if (FinalizeAndExtractPSQT(psqt_mut, result)) {
        const PartiallySignedQuicksilverTransaction psqt_from_tx{result};
    }

    PartiallySignedQuicksilverTransaction psqt_merge;
    str = fuzzed_data_provider.ConsumeRandomLengthString();
    if (!DecodeRawPSQT(psqt_merge, MakeByteSpan(str), error)) {
        psqt_merge = psqt;
    }
    psqt_mut = psqt;
    (void)psqt_mut.Merge(psqt_merge);
    psqt_mut = psqt;
    (void)CombinePSQTs(psqt_mut, {psqt_mut, psqt_merge});
    psqt_mut = psqt;
    for (unsigned int i = 0; i < psqt_merge.tx->vin.size(); ++i) {
        (void)psqt_mut.AddInput(psqt_merge.tx->vin[i], psqt_merge.inputs[i]);
    }
    for (unsigned int i = 0; i < psqt_merge.tx->vout.size(); ++i) {
        Assert(psqt_mut.AddOutput(psqt_merge.tx->vout[i], psqt_merge.outputs[i]));
    }
    psqt_mut.unknown.insert(psqt_merge.unknown.begin(), psqt_merge.unknown.end());
}
