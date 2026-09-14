// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//! @file common/messages.h is a home for simple string functions returning
//! descriptive messages that are used in RPC and GUI interfaces or log
//! messages, and are called in different parts of the codebase across
//! node/vault/gui boundaries.

#ifndef QUICKSILVER_COMMON_MESSAGES_H
#define QUICKSILVER_COMMON_MESSAGES_H

#include <string>

struct bilingual_str;

namespace node {
enum class TransactionError;
} // namespace node

namespace common {
enum class PSQTError;
bilingual_str PSQTErrorString(PSQTError error);
bilingual_str TransactionErrorString(const node::TransactionError error);
bilingual_str ResolveErrMsg(const std::string& optname, const std::string& strBind);
bilingual_str InvalidPortErrMsg(const std::string& optname, const std::string& strPort);
bilingual_str AmountHighWarn(const std::string& optname);
bilingual_str AmountErrMsg(const std::string& optname, const std::string& strValue);
} // namespace common

#endif // QUICKSILVER_COMMON_MESSAGES_H
