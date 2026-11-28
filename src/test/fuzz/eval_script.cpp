// Copyright (c) 2009-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pubkey.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <uint256.h>

#include <cstdint>
#include <limits>
#include <vector>

FUZZ_TARGET(eval_script)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());
    const unsigned int flags = fuzzed_data_provider.ConsumeIntegral<unsigned int>();
    const std::vector<uint8_t> script_bytes = [&] {
        if (fuzzed_data_provider.remaining_bytes() != 0) {
            return fuzzed_data_provider.ConsumeRemainingBytes<uint8_t>();
        } else {
            // Avoid UBSan warning:
            //   test/fuzz/FuzzedDataProvider.h:212:17: runtime error: null pointer passed as argument 1, which is declared to never be null
            //   /usr/include/string.h:43:28: note: nonnull attribute specified here
            return std::vector<uint8_t>();
        }
    }();
    const CScript script(script_bytes.begin(), script_bytes.end());
    for (const auto sig_version : {SigVersion::BASE, SigVersion::WITNESS_V0}) {
        std::vector<std::vector<unsigned char>> stack;
        (void)EvalScript(stack, script, flags, BaseSignatureChecker(), sig_version, nullptr);
    }
    {
        // S0395-A-01: a valid TAPSCRIPT context, set up as VerifyWitnessProgram
        // does for a script-path spend (interpreter.cpp): the tapleaf hash and the
        // validation weight budget are initialized. The budget is derived from the
        // script size (no extra input bytes, so existing seeds keep their layout).
        ScriptExecutionData execdata;
        execdata.m_tapleaf_hash_init = true;
        execdata.m_tapleaf_hash = uint256{};
        execdata.m_validation_weight_left_init = true;
        execdata.m_validation_weight_left = static_cast<int64_t>(script.size()) + VALIDATION_WEIGHT_OFFSET;
        std::vector<std::vector<unsigned char>> stack;
        (void)EvalScript(stack, script, flags, BaseSignatureChecker(), SigVersion::TAPSCRIPT, execdata, nullptr);
    }
}
