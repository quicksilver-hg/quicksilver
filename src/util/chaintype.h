// Copyright (c) 2023 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_UTIL_CHAINTYPE_H
#define QUICKSILVER_UTIL_CHAINTYPE_H

#include <optional>
#include <string>

enum class ChainType {
    MAIN,
    SANDBOX,
    PUBLIC_TEST,
};

std::string ChainTypeToString(ChainType chain);

std::optional<ChainType> ChainTypeFromString(std::string_view chain);

#endif // QUICKSILVER_UTIL_CHAINTYPE_H
