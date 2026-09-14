// Copyright (c) 2017-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_RPC_RELAYPOOL_H
#define QUICKSILVER_RPC_RELAYPOOL_H

class CTxRelayPool;
class UniValue;

/** RelayPool information to JSON */
UniValue RelayPoolInfoToJSON(const CTxRelayPool& pool);

/** RelayPool to JSON */
UniValue RelayPoolToJSON(const CTxRelayPool& pool, bool verbose = false, bool include_relaypool_sequence = false);

#endif // QUICKSILVER_RPC_RELAYPOOL_H
