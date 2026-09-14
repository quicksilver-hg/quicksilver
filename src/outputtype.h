// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_OUTPUTTYPE_H
#define QUICKSILVER_OUTPUTTYPE_H

#include <addresstype.h>
#include <script/signingprovider.h>

#include <array>
#include <optional>
#include <string>

/**
 * How an output's script is presented to a human as an address.
 *
 * **These numbers are a vault-database format, not an internal detail.** The
 * active-ScriptPubKeyMan records store this enum as a raw `uint8_t` key --
 * `DBKeys::ACTIVEEXTERNALSPK` and `ACTIVEINTERNALSPK`, written by
 * `VaultBatch::WriteActiveScriptPubKeyMan` and read straight back through
 * `static_cast<OutputType>` in `vault/vaultdb.cpp`. Renumbering a member re-files
 * every descriptor in every existing vault under a different type, and nothing
 * anywhere reports it: the vault opens, and then address generation either hands
 * back the wrong kind of address or fails deep inside the descriptor.
 *
 * Quicksilver establishes this compact mapping before launch. Give a new member
 * the next unused value and do not renumber the format after launch.
 *
 * The underlying type stays the default: several headers forward-declare this
 * enum opaquely, and an opaque declaration commits to `int`.
 */
enum class OutputType {
    BASE58 = 0,
    BECH32 = 1,
    BECH32M = 2,
    UNKNOWN = 3,
};

static constexpr auto OUTPUT_TYPES = std::array{
    OutputType::BASE58,
    OutputType::BECH32,
    OutputType::BECH32M,
};

std::optional<OutputType> ParseOutputType(const std::string& str);
const std::string& FormatOutputType(OutputType type);

/**
 * Recover an address type from the ordinal a vault file stored.
 *
 * Returns nothing for a value this build has no member for. Callers reject such
 * records rather than casting an uncovered value into the enum.
 */
std::optional<OutputType> OutputTypeFromStored(uint8_t stored);

/**
 * Get a destination of the requested type (if possible) to the specified script.
 * This function will automatically add the script (and any other
 * necessary scripts) to the keystore.
 */
CTxDestination AddAndGetDestinationForScript(FlatSigningProvider& keystore, const CScript& script, OutputType);

/** Get the OutputType for a CTxDestination */
std::optional<OutputType> OutputTypeFromDestination(const CTxDestination& dest);

#endif // QUICKSILVER_OUTPUTTYPE_H
