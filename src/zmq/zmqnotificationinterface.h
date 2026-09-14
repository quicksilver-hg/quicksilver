// Copyright (c) 2015-present The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_ZMQ_ZMQNOTIFICATIONINTERFACE_H
#define QUICKSILVER_ZMQ_ZMQNOTIFICATIONINTERFACE_H

#include <primitives/transaction.h>
#include <validationinterface.h>

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <vector>

class CBlock;
class CBlockIndex;
class CZMQAbstractNotifier;

class CZMQNotificationInterface final : public CValidationInterface
{
public:
    ~CZMQNotificationInterface();

    std::list<const CZMQAbstractNotifier*> GetActiveNotifiers() const;

    static std::unique_ptr<CZMQNotificationInterface> Create(std::function<bool(std::vector<uint8_t>&, const CBlockIndex&)> get_block_by_index);

protected:
    bool Initialize();
    void Shutdown();

    // CValidationInterface
    void TransactionAddedToRelayPool(const CTransactionRef& tx, uint64_t relaypool_sequence) override;
    void TransactionRemovedFromRelayPool(const CTransactionRef& tx, RelayPoolRemovalReason reason, uint64_t relaypool_sequence) override;
    void BlockConnected(const std::shared_ptr<const CBlock>& pblock, const CBlockIndex* pindexConnected) override;
    void BlockDisconnected(const std::shared_ptr<const CBlock>& pblock, const CBlockIndex* pindexDisconnected) override;
    void UpdatedBlockTip(const CBlockIndex *pindexNew, const CBlockIndex *pindexFork, bool fInitialDownload) override;

private:
    CZMQNotificationInterface();

    void* pcontext{nullptr};
    std::list<std::unique_ptr<CZMQAbstractNotifier>> notifiers;
};

extern std::unique_ptr<CZMQNotificationInterface> g_zmq_notification_interface;

#endif // QUICKSILVER_ZMQ_ZMQNOTIFICATIONINTERFACE_H
