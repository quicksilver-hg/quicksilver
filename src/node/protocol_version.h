// Copyright (c) 2012-present The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_NODE_PROTOCOL_VERSION_H
#define QUICKSILVER_NODE_PROTOCOL_VERSION_H

/**
 * network protocol versioning
 */

static const int PROTOCOL_VERSION = 70016;

//! initial proto version, to be increased after version/verack negotiation
static const int INIT_PROTO_VERSION = 209;

//! disconnect from peers older than this proto version
static const int MIN_PEER_PROTO_VERSION = PROTOCOL_VERSION;

#endif // QUICKSILVER_NODE_PROTOCOL_VERSION_H
