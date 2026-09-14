// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_OVERVIEWPAGE_H
#define QUICKSILVER_QT_OVERVIEWPAGE_H

#include <interfaces/vault.h>

#include <QWidget>
#include <memory>

class ClientModel;
class TransactionFilterProxy;
class TxViewDelegate;
class PlatformStyle;
class VaultModel;

namespace Ui {
    class OverviewPage;
}

QT_BEGIN_NAMESPACE
class QModelIndex;
class QFrame;
class QLabel;
class QPushButton;
QT_END_NAMESPACE

/** Overview ("home") page widget */
class OverviewPage : public QWidget
{
    Q_OBJECT

public:
    explicit OverviewPage(const PlatformStyle *platformStyle, QWidget *parent = nullptr);
    ~OverviewPage();

    void setClientModel(ClientModel *clientModel);
    void setVaultModel(VaultModel *vaultModel);
    void showOutOfSyncWarning(bool fShow);
    void setBackupState(bool backup_done);

public Q_SLOTS:
    void setBalance(const interfaces::VaultBalances& balances);
    void setPrivacy(bool privacy);

Q_SIGNALS:
    void transactionClicked(const QModelIndex &index);
    void outOfSyncWarningClicked();
    void backupRequested();

protected:
    void changeEvent(QEvent* e) override;

private:
    Ui::OverviewPage *ui;
    ClientModel* clientModel{nullptr};
    VaultModel* vaultModel{nullptr};
    bool m_privacy{false};

    const PlatformStyle* m_platform_style;
    QFrame* m_backup_panel{nullptr};
    QLabel* m_backup_state{nullptr};
    QLabel* m_backup_summary{nullptr};
    QPushButton* m_backup_button{nullptr};

    TxViewDelegate *txdelegate;
    std::unique_ptr<TransactionFilterProxy> filter;

private Q_SLOTS:
    void updateMaturityCountdown();
    void LimitTransactionRows();
    void updateDisplayUnit();
    void handleTransactionClicked(const QModelIndex &index);
    void updateAlerts(const QString &warnings);
    void setMonospacedFont(const QFont&);
};

#endif // QUICKSILVER_QT_OVERVIEWPAGE_H
