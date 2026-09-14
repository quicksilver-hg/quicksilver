// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <outputtype.h>

#include <script/script.h>
#include <script/signingprovider.h>

#include <assert.h>
#include <optional>
#include <string>

static const std::string OUTPUT_TYPE_STRING_BASE58 = "base58";
static const std::string OUTPUT_TYPE_STRING_BECH32 = "bech32";
static const std::string OUTPUT_TYPE_STRING_BECH32M = "bech32m";
static const std::string OUTPUT_TYPE_STRING_UNKNOWN = "unknown";

std::optional<OutputType> ParseOutputType(const std::string& type)
{
    if (type == OUTPUT_TYPE_STRING_BASE58) {
        return OutputType::BASE58;
    } else if (type == OUTPUT_TYPE_STRING_BECH32) {
        return OutputType::BECH32;
    } else if (type == OUTPUT_TYPE_STRING_BECH32M) {
        return OutputType::BECH32M;
    }
    return std::nullopt;
}

std::optional<OutputType> OutputTypeFromStored(uint8_t stored)
{
    for (const OutputType type : OUTPUT_TYPES) {
        if (static_cast<uint8_t>(type) == stored) return type;
    }
    if (stored == static_cast<uint8_t>(OutputType::UNKNOWN)) return OutputType::UNKNOWN;
    return std::nullopt;
}

const std::string& FormatOutputType(OutputType type)
{
    switch (type) {
    case OutputType::BASE58: return OUTPUT_TYPE_STRING_BASE58;
    case OutputType::BECH32: return OUTPUT_TYPE_STRING_BECH32;
    case OutputType::BECH32M: return OUTPUT_TYPE_STRING_BECH32M;
    case OutputType::UNKNOWN: return OUTPUT_TYPE_STRING_UNKNOWN;
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

CTxDestination AddAndGetDestinationForScript(FlatSigningProvider& keystore, const CScript& script, OutputType type)
{
    // Add script to keystore
    keystore.scripts.emplace(CScriptID(script), script);

    switch (type) {
    case OutputType::BASE58:
        return ScriptHash(script);
    case OutputType::BECH32: {
        CTxDestination witdest = WitnessV0ScriptHash(script);
        return witdest;
    }
    case OutputType::BECH32M:
    case OutputType::UNKNOWN: {} // This function should not be used for BECH32M or UNKNOWN, so let it assert
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

std::optional<OutputType> OutputTypeFromDestination(const CTxDestination& dest) {
    if (std::holds_alternative<PKHash>(dest) ||
        std::holds_alternative<ScriptHash>(dest)) {
        return OutputType::BASE58;
    }
    if (std::holds_alternative<WitnessV0KeyHash>(dest) ||
        std::holds_alternative<WitnessV0ScriptHash>(dest)) {
        return OutputType::BECH32;
    }
    if (std::holds_alternative<WitnessV1Taproot>(dest) ||
        std::holds_alternative<WitnessUnknown>(dest)) {
        return OutputType::BECH32M;
    }
    return std::nullopt;
}
