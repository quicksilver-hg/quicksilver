// Copyright (c) 2018-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_INTERFACES_CHAIN_H
#define QUICKSILVER_INTERFACES_CHAIN_H

#include <blockfilter.h>
#include <common/settings.h>
#include <node/types.h>
#include <primitives/transaction.h> // For CTransactionRef
#include <util/result.h>

#include <functional>
#include <memory>
#include <optional>
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

class ArgsManager;
class CBlock;
class CRPCCommand;
class CScheduler;
class Coin;
class uint256;
enum class RelayPoolRemovalReason;
struct bilingual_str;
struct CBlockLocator;
namespace node {
struct NodeContext;
} // namespace node

namespace interfaces {

class Handler;
class Vault;

//! Helper for findBlock to selectively return pieces of block data. If block is
//! found, data will be returned by setting specified output variables. If block
//! is not found, output variables will keep their previous values.
class FoundBlock
{
public:
    FoundBlock& hash(uint256& hash) { m_hash = &hash; return *this; }
    FoundBlock& height(int& height) { m_height = &height; return *this; }
    FoundBlock& time(int64_t& time) { m_time = &time; return *this; }
    FoundBlock& maxTime(int64_t& max_time) { m_max_time = &max_time; return *this; }
    FoundBlock& mtpTime(int64_t& mtp_time) { m_mtp_time = &mtp_time; return *this; }
    //! Return whether block is in the active (most-work) chain.
    FoundBlock& inActiveChain(bool& in_active_chain) { m_in_active_chain = &in_active_chain; return *this; }
    //! Return locator if block is in the active chain.
    FoundBlock& locator(CBlockLocator& locator) { m_locator = &locator; return *this; }
    //! Return next block in the active chain if current block is in the active chain.
    FoundBlock& nextBlock(const FoundBlock& next_block) { m_next_block = &next_block; return *this; }
    //! Read block data from disk. If the block exists but doesn't have data
    //! (for example due to pruning), the CBlock variable will be set to null.
    FoundBlock& data(CBlock& data) { m_data = &data; return *this; }

    uint256* m_hash = nullptr;
    int* m_height = nullptr;
    int64_t* m_time = nullptr;
    int64_t* m_max_time = nullptr;
    int64_t* m_mtp_time = nullptr;
    bool* m_in_active_chain = nullptr;
    CBlockLocator* m_locator = nullptr;
    const FoundBlock* m_next_block = nullptr;
    CBlock* m_data = nullptr;
    mutable bool found = false;
};

//! Block data sent with blockConnected, blockDisconnected notifications.
struct BlockInfo {
    const uint256& hash;
    const uint256* prev_hash = nullptr;
    int height = -1;
    int file_number = -1;
    unsigned data_pos = 0;
    const CBlock* data = nullptr;
    // The maximum time in the chain up to and including this block.
    // A timestamp that can only move forward.
    unsigned int chain_time_max{0};

    BlockInfo(const uint256& hash LIFETIMEBOUND) : hash(hash) {}
};

//! The action to be taken after updating a settings value.
//! WRITE indicates that the updated value must be written to disk,
//! while SKIP_WRITE indicates that the change will be kept in memory-only
//! without persisting it.
enum class SettingsAction {
    WRITE,
    SKIP_WRITE
};

using SettingsUpdate = std::function<std::optional<interfaces::SettingsAction>(common::SettingsValue&)>;

//! Interface giving clients (vault processes, maybe other analysis tools in
//! the future) ability to access to the chain state, receive notifications,
//!  and submit transactions.
//!
//! TODO: Current chain methods are too low level, exposing too much of the
//! internal workings of the Quicksilver node, and not being very convenient to use.
//! Chain methods should be cleaned up and simplified over time. Examples:
//!
//! * The initMessages() and showProgress() methods which the vault uses to send
//!   notifications to the GUI should go away when GUI and vault can directly
//!   communicate with each other without going through the node.
//!
//! * The handleRpc, registerRpcs, and other RPC
//!   methods can go away if vaults listen for HTTP requests on their own
//!   ports instead of registering to handle requests on the node HTTP port.
//!
//! * `guessVerificationProgress` and similar methods can go away if rescan
//!   logic moves out of the vault, and the vault just requests scans from the
//!   node.
class Chain
{
public:
    virtual ~Chain() = default;

    //! Get current chain height, not including genesis block (returns 0 if
    //! chain only contains genesis block, nullopt if chain does not contain
    //! any blocks)
    virtual std::optional<int> getHeight() = 0;

    //! Get block hash. Height must be valid or this function will abort.
    virtual uint256 getBlockHash(int height) = 0;

    //! Quicksilver Stage 2: the per-tx PoW target a transaction of this shape must
    //! meet when anchored at `anchor_height`. Required work is per-transaction after
    //! the flag day, and a sender grinds before its signature bytes exist, so it
    //! passes an UPPER BOUND on its final serialized size here. Returns nullopt if
    //! the height is not on the active chain. Routed through the interface rather
    //! than recomputed in the vault so that sender and consensus cannot disagree
    //! about the arithmetic. Height must be valid or this function will abort.
    virtual uint256 txPowTarget(int anchor_height, uint64_t bytes, size_t nout, size_t nin) = 0;

    //! Check that the block is available on disk (i.e. has not been
    //! pruned), and contains transactions.
    virtual bool haveBlockOnDisk(int height) = 0;

    //! Get locator for the current chain tip.
    virtual CBlockLocator getTipLocator() = 0;

    //! Return a locator that refers to a block in the active chain.
    //! If specified block is not in the active chain, return locator for the latest ancestor that is in the chain.
    virtual CBlockLocator getActiveChainLocator(const uint256& block_hash) = 0;

    //! Return height of the highest block on chain in common with the locator,
    //! which will either be the original block used to create the locator,
    //! or one of its ancestors.
    virtual std::optional<int> findLocatorFork(const CBlockLocator& locator) = 0;

    //! Returns whether a block filter index is available.
    virtual bool hasBlockFilterIndex(BlockFilterType filter_type) = 0;

    //! Returns whether any of the elements match the block via a BIP 157 block filter
    //! or std::nullopt if the block filter for this block couldn't be found.
    virtual std::optional<bool> blockFilterMatchesAny(BlockFilterType filter_type, const uint256& block_hash, const GCSFilter::ElementSet& filter_set) = 0;

    //! Return whether node has the block and optionally return block metadata
    //! or contents.
    virtual bool findBlock(const uint256& hash, const FoundBlock& block={}) = 0;

    //! Find first block in the chain with timestamp >= the given time
    //! and height >= than the given height, return false if there is no block
    //! with a high enough timestamp and height. Optionally return block
    //! information.
    virtual bool findFirstBlockWithTimeAndHeight(int64_t min_time, int min_height, const FoundBlock& block={}) = 0;

    //! Find ancestor of block at specified height and optionally return
    //! ancestor information.
    virtual bool findAncestorByHeight(const uint256& block_hash, int ancestor_height, const FoundBlock& ancestor_out={}) = 0;

    //! Return whether block descends from a specified ancestor, and
    //! optionally return ancestor information.
    virtual bool findAncestorByHash(const uint256& block_hash,
        const uint256& ancestor_hash,
        const FoundBlock& ancestor_out={}) = 0;

    //! Find most recent common ancestor between two blocks and optionally
    //! return block information.
    virtual bool findCommonAncestor(const uint256& block_hash1,
        const uint256& block_hash2,
        const FoundBlock& ancestor_out={},
        const FoundBlock& block1_out={},
        const FoundBlock& block2_out={}) = 0;

    //! Look up unspent output information. Returns coins in the relaypool and in
    //! the current chain UTXO set. Iterates through all the keys in the map and
    //! populates the values.
    virtual void findCoins(std::map<COutPoint, Coin>& coins) = 0;

    //! Estimate fraction of total transactions verified if blocks up to
    //! the specified block hash are verified.
    virtual double guessVerificationProgress(const uint256& block_hash) = 0;

    //! Return true if data is available for all blocks in the specified range
    //! of blocks. This checks all blocks that are ancestors of block_hash in
    //! the height range from min_height to max_height, inclusive.
    virtual bool hasBlocks(const uint256& block_hash, int min_height = 0, std::optional<int> max_height = {}) = 0;

    //! Check if transaction is in relaypool.
    virtual bool isInRelayPool(const uint256& txid) = 0;

    //! Transaction is added to memory pool and broadcast to all peers if relay is set to true.
    //! Return the node's classified submission result.
    virtual node::TransactionError broadcastTransaction(const CTransactionRef& tx,
                                                        bool relay,
                                                        std::string& err_string) = 0;

    //! Calculate relaypool ancestor and descendant counts for the given transaction.
    virtual void getTransactionAncestry(const uint256& txid, size_t& ancestors, size_t& descendants, size_t* ancestorsize = nullptr) = 0;

    //! Get the node's package limits.
    //! Currently only returns the ancestor and descendant count limits, but could be enhanced to
    //! return more policy settings.
    virtual void getPackageLimits(unsigned int& limit_ancestor_count, unsigned int& limit_descendant_count) = 0;

    //! Check if transaction will pass the relaypool's chain limits.
    virtual util::Result<void> checkChainLimits(const CTransactionRef& tx) = 0;

    //! Check if any block has been pruned.
    virtual bool havePruned() = 0;

    //! Get the current prune height.
    virtual std::optional<int> getPruneHeight() = 0;

    //! Check if the node is ready to broadcast transactions.
    virtual bool isReadyToBroadcast() = 0;

    //! Check if in IBD.
    virtual bool isInitialBlockDownload() = 0;

    //! Check if shutdown requested.
    virtual bool shutdownRequested() = 0;

    //! Send init message.
    virtual void initMessage(const std::string& message) = 0;

    //! Send init warning.
    virtual void initWarning(const bilingual_str& message) = 0;

    //! Send init error.
    virtual void initError(const bilingual_str& message) = 0;

    //! Send progress indicator.
    virtual void showProgress(const std::string& title, int progress, bool resume_possible) = 0;

    //! Chain notifications.
    class Notifications
    {
    public:
        virtual ~Notifications() = default;
        virtual void transactionAddedToRelayPool(const CTransactionRef& tx) {}
        virtual void transactionRemovedFromRelayPool(const CTransactionRef& tx, RelayPoolRemovalReason reason) {}
        virtual void blockConnected(const BlockInfo& block) {}
        virtual void blockDisconnected(const BlockInfo& block) {}
        virtual void updatedBlockTip() {}
        virtual void chainStateFlushed(const CBlockLocator& locator) {}
    };

    //! Register handler for notifications.
    virtual std::unique_ptr<Handler> handleNotifications(std::shared_ptr<Notifications> notifications) = 0;

    //! Wait for pending notifications to be processed unless block hash points to the current
    //! chain tip.
    virtual void waitForNotificationsIfTipChanged(const uint256& old_tip) = 0;

    //! Register handler for RPC. Command is not copied, so reference
    //! needs to remain valid until Handler is disconnected.
    virtual std::unique_ptr<Handler> handleRpc(const CRPCCommand& command) = 0;

    //! Run function after given number of seconds. Cancel any previous calls with same name.
    virtual void rpcRunLater(const std::string& name, std::function<void()> fn, int64_t seconds) = 0;

    //! Get list of settings values.
    virtual std::vector<common::SettingsValue> getSettingsList(const std::string& arg) = 0;

    //! Return <datadir>/settings.json setting value.
    virtual common::SettingsValue getRwSetting(const std::string& name) = 0;

    //! Updates a setting in <datadir>/settings.json.
    //! Null can be passed to erase the setting. There is intentionally no
    //! support for writing null values to settings.json.
    //! Depending on the action returned by the update function, this will either
    //! update the setting in memory or write the updated settings to disk.
    virtual bool updateRwSetting(const std::string& name, const SettingsUpdate& update_function) = 0;

    //! Synchronously send transactionAddedToRelayPool notifications about all
    //! current relaypool transactions to the specified handler and return after
    //! the last one is sent. These notifications aren't coordinated with async
    //! notifications sent by handleNotifications, so out of date async
    //! notifications from handleNotifications can arrive during and after
    //! synchronous notifications from requestRelayPoolTransactions. Clients need
    //! to be prepared to handle this by ignoring notifications about unknown
    //! removed transactions and already added new transactions.
    virtual void requestRelayPoolTransactions(Notifications& notifications) = 0;

    //! Whether a chainstate exists behind this interface at all. False in the
    //! consensus-off shell, where the vault holds a live Chain pointer but the
    //! node never built a ChainstateManager. Every other method on this
    //! interface dereferences the chainstate and aborts when it is absent, so a
    //! non-null Chain* is not on its own evidence that the chain can be used.
    virtual bool hasChainstate() = 0;

    //! Get internal node context. Useful for testing, but not
    //! accessible across processes.
    virtual node::NodeContext* context() { return nullptr; }
};

//! Interface to let node manage chain clients (vaults, or maybe tools for
//! monitoring and analysis in the future).
class ChainClient
{
public:
    virtual ~ChainClient() = default;

    //! Register rpcs.
    virtual void registerRpcs() = 0;

    //! Check for errors before loading.
    virtual bool verify() = 0;

    //! Load saved state.
    virtual bool load() = 0;

    //! Start client execution and provide a scheduler.
    virtual void start(CScheduler& scheduler) = 0;

    //! Save state to disk.
    virtual void flush() = 0;

    //! Shut down client.
    virtual void stop() = 0;

    //! Set mock time.
    virtual void setMockTime(int64_t time) = 0;

    //! Mock the scheduler to fast forward in time.
    virtual void schedulerMockForward(std::chrono::seconds delta_seconds) = 0;
};

//! Return implementation of Chain interface.
std::unique_ptr<Chain> MakeChain(node::NodeContext& node);

} // namespace interfaces

#endif // QUICKSILVER_INTERFACES_CHAIN_H
