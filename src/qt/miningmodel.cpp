// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/miningmodel.h>

#include <interfaces/vault.h>
#include <key_io.h>
#include <outputtype.h>
#include <qt/vaultmodel.h>
#include <util/result.h>

MiningModel::MiningModel(interfaces::Node& node, VaultModel* vault_model, QObject* parent)
    : QObject(parent), m_node(node), m_vault_model(vault_model)
{
    m_timer.setInterval(1000);
    connect(&m_timer, &QTimer::timeout, this, &MiningModel::refresh);
    m_timer.start();
}

void MiningModel::refresh()
{
    Q_EMIT statusUpdated(m_node.miningStatus());
}

void MiningModel::setVaultModel(VaultModel* vault_model)
{
    m_vault_model = vault_model;
}

void MiningModel::start(const QString& address)
{
    if (auto res = m_node.startMining(address.toStdString()); !res) {
        Q_EMIT miningError(QString::fromStdString(util::ErrorString(res).original));
        return;
    }
    refresh();
}

void MiningModel::stop()
{
    m_node.stopMining();
    refresh();
}

QString MiningModel::freshPayoutAddress()
{
    if (!m_vault_model) return QString();
    auto dest = m_vault_model->vault().getNewDestination(OutputType::BECH32, "mining");
    if (!dest) return QString();
    return QString::fromStdString(EncodeDestination(*dest));
}
