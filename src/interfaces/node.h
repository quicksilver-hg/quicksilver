// Copyright (c) 2018-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_INTERFACES_NODE_H
#define QUICKSILVER_INTERFACES_NODE_H

#include <common/settings.h>
#include <consensus/amount.h>          // For CAmount
#include <logging.h>                   // For HgLog::CategoryMask
#include <net.h>                       // For NodeId
#include <net_types.h>                 // For banmap_t
#include <netaddress.h>                // For Network
#include <netbase.h>                   // For ConnectionDirection
#include <support/allocators/secure.h> // For SecureString
#include <util/result.h>
#include <util/translation.h>

#include <functional>
#include <memory>
#include <optional>
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <tuple>
#include <vector>

class BanMan;
class CNodeStats;
class Coin;
class RPCTimerInterface;
class UniValue;
class Proxy;
enum class SynchronizationState;
struct CNodeStateStats;
struct bilingual_str;
namespace node {
enum class TransactionError;
struct NodeContext;
} // namespace node
namespace vault {
class CCoinControl;
} // namespace vault

namespace interfaces {
class Handler;
class VaultLoader;
struct BlockTip;

//! Block and header tip information
struct BlockAndHeaderTipInfo
{
    int block_height;
    int64_t block_time;
    int header_height;
    int64_t header_time;
    double verification_progress;
};

//! Snapshot of the background mining role for GUI/RPC consumers.
struct MiningStatus {
    bool active{false};
    std::string address;                 //!< payout address (empty if never started)
    int64_t blocks_found{0};
    CAmount coins_minted_session{0};
    //! Solver attempts per second over the trailing 120 s, NOT a session average:
    //! the average could not distinguish a card that died an hour in from one that
    //! never stopped, and it read low for as long as the arming/spawn dead time was
    //! a meaningful share of the session. Unset means no attempt has been observed
    //! this session -- render that as a warming-up state, never as zero.
    std::optional<double> attempts_per_second;
    //! Chain-dependent base for transaction PoW at the current tip, in Cuckatoo
    //! cycles. This is the day-one cost of sending a transaction on a feeless chain,
    //! so it is reported alongside mining telemetry rather than buried in a debug
    //! RPC. Stage 2 made required work per-transaction, so a sender's actual bill is
    //! this scaled by their own serialized bytes and net UTXO creation -- there is no
    //! single "required work" at a tip to publish any more. See RequiredTxWork.
    double base_tx_work{1.0};
    int64_t last_block_time{0};
    int64_t elapsed_seconds{0};
    double congestion_multiplier{1.0};   //!< 1.0 == per-tx PoW hardware floor
    bool gpu_solver{false};              //!< true when this process has a GPU solver bridge
    //! Monotonic count of Cuckatoo graphs attempted this session. Reported
    //! alongside the rate because it is the session total the rate deliberately is
    //! not: it distinguishes a solver that has never produced anything from one
    //! that has gone quiet, which a rate near zero cannot. Together with solver_ok
    //! it separates "not yet", "no solver" and "solver dead". One E28 graph is
    //! ~1.6 s on the P104-100 reference card, so the first one lands seconds after
    //! arming.
    uint64_t graphs_attempted{0};
    bool solver_ok{true};                //!< false when the last solver attempt faulted
    std::string last_solver_error;       //!< empty when solver_ok
    //! The fault is "no solver configured" rather than a solver that ran and failed.
    //! last_solver_error is written for quicksilverd and names the -cuckatoosolver
    //! flag; the GUI has the setting as a control and must say so instead.
    bool solver_missing{false};
    //! The block this node is grinding right now. Distinct from getmininginfo's
    //! currentblocktx, which reports whatever was assembled last by any caller.
    int64_t template_height{0};          //!< 0 when nothing is being ground
    int64_t template_transactions{0};    //!< transfers in it, excluding the coinbase
};

//! External signer interface used by the GUI.
class ExternalSigner
{
public:
    virtual ~ExternalSigner() = default;

    //! Get signer display name
    virtual std::string getName() = 0;
};

//! Top-level interface for a Quicksilver node (quicksilverd process).
class Node
{
public:
    virtual ~Node() = default;

    //! Init parameter interaction.
    virtual void initParameterInteraction() = 0;

    //! Get warnings.
    virtual bilingual_str getWarnings() = 0;

    //! Get exit status.
    virtual int getExitStatus() = 0;

    // Get log flags.
    virtual HgLog::CategoryMask getLogCategories() = 0;

    //! Initialize app dependencies.
    virtual bool baseInitialize() = 0;

    //! Start node.
    virtual bool appInitMain(interfaces::BlockAndHeaderTipInfo* tip_info = nullptr) = 0;

    //! Stop node.
    virtual void appShutdown() = 0;

    //! Start shutdown.
    virtual void startShutdown() = 0;

    //! Return whether shutdown was requested.
    virtual bool shutdownRequested() = 0;

    //! Return whether a particular setting in <datadir>/settings.json is or
    //! would be ignored because it is also specified in the command line.
    virtual bool isSettingIgnored(const std::string& name) = 0;

    //! Return setting value from <datadir>/settings.json or quicksilver.conf.
    virtual common::SettingsValue getPersistentSetting(const std::string& name) = 0;

    //! Update a setting in <datadir>/settings.json.
    virtual void updateRwSetting(const std::string& name, const common::SettingsValue& value) = 0;

    //! Force a setting value to be applied, overriding any other configuration
    //! source, but not being persisted.
    virtual void forceSetting(const std::string& name, const common::SettingsValue& value) = 0;

    //! Clear all settings in <datadir>/settings.json and store a backup of
    //! previous settings in <datadir>/settings.json.bak.
    virtual void resetSettings() = 0;

    //! Map port.
    virtual void mapPort(bool enable) = 0;

    //! Get proxy.
    virtual bool getProxy(Network net, Proxy& proxy_info) = 0;

    //! Get number of connections.
    virtual size_t getNodeCount(ConnectionDirection flags) = 0;

    //! Get stats for connected nodes.
    using NodesStats = std::vector<std::tuple<CNodeStats, bool, CNodeStateStats>>;
    virtual bool getNodesStats(NodesStats& stats) = 0;

    //! Get known node addresses from addrman, filtered by the node's quality and recency rules.
    virtual std::vector<CService> getNodeAddresses(size_t count) = 0;

    //! Get ban map entries.
    virtual bool getBanned(banmap_t& banmap) = 0;

    //! Ban node.
    virtual bool ban(const CNetAddr& net_addr, int64_t ban_time_offset) = 0;

    //! Unban node.
    virtual bool unban(const CSubNet& ip) = 0;

    //! Add a peer to the manual connection list, the -addnode equivalent.
    //!
    //! Quicksilver ships no DNS seeds and a single onion fixed seed, so a node with
    //! no Tor has nothing it can dial: naming a peer directly is the only route to
    //! the network left, and it needs to be reachable from a desktop that has no
    //! config file and no terminal. Returns false when the address is already on the
    //! list or no connection manager is running. Success means only that the address
    //! was accepted for retrying -- whether it answers is visible in the peer count.
    virtual bool addNode(const std::string& address) = 0;

    //! Disconnect node by address.
    virtual bool disconnectByAddress(const CNetAddr& net_addr) = 0;

    //! Disconnect node by id.
    virtual bool disconnectById(NodeId id) = 0;

    //! Return list of external signers (attached devices which can sign transactions).
    virtual std::vector<std::unique_ptr<ExternalSigner>> listExternalSigners() = 0;

    //! Get total bytes recv.
    virtual int64_t getTotalBytesRecv() = 0;

    //! Get total bytes sent.
    virtual int64_t getTotalBytesSent() = 0;

    //! Get relaypool size.
    virtual size_t getRelayPoolSize() = 0;

    //! Get relaypool dynamic usage.
    virtual size_t getRelayPoolDynamicUsage() = 0;

    //! Get relaypool maximum memory usage.
    virtual size_t getRelayPoolMaxUsage() = 0;

    //! Get header tip height and time.
    virtual bool getHeaderTip(int& height, int64_t& block_time) = 0;

    //! Get num blocks.
    virtual int getNumBlocks() = 0;

    //! Get network local addresses.
    virtual std::map<CNetAddr, LocalServiceInfo> getNetLocalAddresses() = 0;

    //! Get best block hash.
    virtual uint256 getBestBlockHash() = 0;

    //! Get verification progress.
    virtual double getVerificationProgress() = 0;

    //! Is initial block download.
    virtual bool isInitialBlockDownload() = 0;

    //! Start the opt-in background mining role on an explicit payout address.
    virtual util::Result<void> startMining(const std::string& payout_address) = 0;

    //! Stop the background mining role (no-op if inactive).
    virtual void stopMining() = 0;

    //! Snapshot of the mining role + session mint telemetry.
    virtual MiningStatus miningStatus() = 0;

    //! Is loading blocks.
    virtual bool isLoadingBlocks() = 0;

    //! Set network active.
    virtual void setNetworkActive(bool active) = 0;

    //! Get network active.
    virtual bool getNetworkActive() = 0;

    //! Execute rpc command.
    virtual UniValue executeRpc(const std::string& command, const UniValue& params, const std::string& uri) = 0;

    //! List rpc commands.
    virtual std::vector<std::string> listRpcCommands() = 0;

    //! Set RPC timer interface if unset.
    virtual void rpcSetTimerInterfaceIfUnset(RPCTimerInterface* iface) = 0;

    //! Unset RPC timer interface.
    virtual void rpcUnsetTimerInterface(RPCTimerInterface* iface) = 0;

    //! Get unspent output associated with a transaction.
    virtual std::optional<Coin> getUnspentOutput(const COutPoint& output) = 0;

    //! Broadcast transaction.
    virtual node::TransactionError broadcastTransaction(CTransactionRef tx, std::string& err_string) = 0;

    //! Get vault loader.
    virtual VaultLoader& vaultLoader() = 0;

    //! Register handler for init messages.
    using InitMessageFn = std::function<void(const std::string& message)>;
    virtual std::unique_ptr<Handler> handleInitMessage(InitMessageFn fn) = 0;

    //! Register handler for message box messages.
    using MessageBoxFn =
        std::function<bool(const bilingual_str& message, const std::string& caption, unsigned int style)>;
    virtual std::unique_ptr<Handler> handleMessageBox(MessageBoxFn fn) = 0;

    //! Register handler for question messages.
    using QuestionFn = std::function<bool(const bilingual_str& message,
        const std::string& non_interactive_message,
        const std::string& caption,
        unsigned int style)>;
    virtual std::unique_ptr<Handler> handleQuestion(QuestionFn fn) = 0;

    //! Register handler for progress messages.
    using ShowProgressFn = std::function<void(const std::string& title, int progress, bool resume_possible)>;
    virtual std::unique_ptr<Handler> handleShowProgress(ShowProgressFn fn) = 0;

    //! Register handler for vault loader constructed messages.
    using InitVaultFn = std::function<void()>;
    virtual std::unique_ptr<Handler> handleInitVault(InitVaultFn fn) = 0;

    //! Register handler for number of connections changed messages.
    using NotifyNumConnectionsChangedFn = std::function<void(int new_num_connections)>;
    virtual std::unique_ptr<Handler> handleNotifyNumConnectionsChanged(NotifyNumConnectionsChangedFn fn) = 0;

    //! Register handler for network active messages.
    using NotifyNetworkActiveChangedFn = std::function<void(bool network_active)>;
    virtual std::unique_ptr<Handler> handleNotifyNetworkActiveChanged(NotifyNetworkActiveChangedFn fn) = 0;

    //! Register handler for notify alert messages.
    using NotifyAlertChangedFn = std::function<void()>;
    virtual std::unique_ptr<Handler> handleNotifyAlertChanged(NotifyAlertChangedFn fn) = 0;

    //! Register handler for ban list messages.
    using BannedListChangedFn = std::function<void()>;
    virtual std::unique_ptr<Handler> handleBannedListChanged(BannedListChangedFn fn) = 0;

    //! Register handler for block tip messages.
    using NotifyBlockTipFn =
        std::function<void(SynchronizationState, interfaces::BlockTip tip, double verification_progress)>;
    virtual std::unique_ptr<Handler> handleNotifyBlockTip(NotifyBlockTipFn fn) = 0;

    //! Register handler for header tip messages.
    using NotifyHeaderTipFn =
        std::function<void(SynchronizationState, interfaces::BlockTip tip, bool presync)>;
    virtual std::unique_ptr<Handler> handleNotifyHeaderTip(NotifyHeaderTipFn fn) = 0;

    //! Get and set internal node context. Useful for testing, but not
    //! accessible across processes.
    virtual node::NodeContext* context() { return nullptr; }
    virtual void setContext(node::NodeContext* context) { }
};

//! Return implementation of Node interface.
std::unique_ptr<Node> MakeNode(node::NodeContext& context);

//! Block tip (could be a header or not, depends on the subscribed signal).
struct BlockTip {
    int block_height;
    int64_t block_time;
    uint256 block_hash;
};

} // namespace interfaces

#endif // QUICKSILVER_INTERFACES_NODE_H
