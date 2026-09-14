// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_MININGMODEL_H
#define QUICKSILVER_QT_MININGMODEL_H

#include <interfaces/node.h>

#include <QObject>
#include <QString>
#include <QTimer>

class VaultModel;

/** Polls the node's opt-in mining role once a second and surfaces control slots.
 *
 * Reads status through interfaces::Node so the GUI never touches node internals;
 * failures from start() are reported via the miningError signal.
 */
class MiningModel : public QObject
{
    Q_OBJECT

public:
    MiningModel(interfaces::Node& node, VaultModel* vault_model, QObject* parent = nullptr);

    void refresh(); //!< pull status now and emit statusUpdated
    void setVaultModel(VaultModel* vault_model);

public Q_SLOTS:
    void start(const QString& address); //!< surfaces failures via miningError
    void stop();
    QString freshPayoutAddress(); //!< vault getNewDestination, "" if unavailable

Q_SIGNALS:
    void statusUpdated(const interfaces::MiningStatus& status);
    void miningError(const QString& message);

private:
    interfaces::Node& m_node;
    VaultModel* m_vault_model;
    QTimer m_timer;
};

#endif // QUICKSILVER_QT_MININGMODEL_H
