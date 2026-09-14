// Copyright (c) 2011-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_RECEIVECOINSDIALOG_H
#define QUICKSILVER_QT_RECEIVECOINSDIALOG_H

#include <outputtype.h>
#include <qt/guiutil.h>

#include <QDialog>
#include <QHeaderView>
#include <QItemSelection>
#include <QKeyEvent>
#include <QMenu>
#include <QPoint>
#include <QVariant>

class PlatformStyle;
class VaultModel;

namespace Ui {
    class ReceiveCoinsDialog;
}

QT_BEGIN_NAMESPACE
class QModelIndex;
QT_END_NAMESPACE

/** Dialog for requesting payment of coins */
class ReceiveCoinsDialog : public QDialog
{
    Q_OBJECT

public:
    enum ColumnWidths {
        DATE_COLUMN_WIDTH = 130,
        LABEL_COLUMN_WIDTH = 120,
        AMOUNT_MINIMUM_COLUMN_WIDTH = 180,
        MINIMUM_COLUMN_WIDTH = 130
    };

    explicit ReceiveCoinsDialog(const PlatformStyle *platformStyle, QWidget *parent = nullptr);
    ~ReceiveCoinsDialog();

    void setModel(VaultModel *model);

public Q_SLOTS:
    void clear();
    void reject() override;
    void accept() override;

private:
    Ui::ReceiveCoinsDialog *ui;
    VaultModel* model{nullptr};
    QMenu *contextMenu;
    QAction* copyLabelAction;
    QAction* copyMessageAction;
    QAction* copyAmountAction;
    const PlatformStyle *platformStyle;

    QModelIndex selectedRow();
    void copyColumnToClipboard(int column);
    void finishReceiveRequest(const QString& address, const QString& label, OutputType address_type);

private Q_SLOTS:
    void on_receiveButton_clicked();
    void on_showRequestButton_clicked();
    void on_removeRequestButton_clicked();
    void on_recentRequestsView_doubleClicked(const QModelIndex &index);
    void recentRequestsView_selectionChanged(const QItemSelection &selected, const QItemSelection &deselected);
    void updateDisplayUnit();
    void showMenu(const QPoint &point);
    void copyURI();
    void copyAddress();
    void copyLabel();
    void copyMessage();
    void copyAmount();
};

#endif // QUICKSILVER_QT_RECEIVECOINSDIALOG_H
