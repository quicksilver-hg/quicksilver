// Copyright (c) 2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_KERNEL_CHAIN_H
#define QUICKSILVER_KERNEL_CHAIN_H

class CBlock;
class CBlockIndex;
namespace interfaces {
struct BlockInfo;
} // namespace interfaces

namespace kernel {
//! Return data from block index.
interfaces::BlockInfo MakeBlockInfo(const CBlockIndex* block_index, const CBlock* data = nullptr);

} // namespace kernel

#endif // QUICKSILVER_KERNEL_CHAIN_H
