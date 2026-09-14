// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/overviewpage.h>
#include <qt/forms/ui_overviewpage.h>

#include <qt/quicksilverunits.h>
#include <qt/clientmodel.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/quicksilverstyle.h>
#include <qt/transactionfilterproxy.h>
#include <qt/transactionoverviewwidget.h>
#include <qt/transactiontablemodel.h>
#include <chainparams.h>
#include <qt/maturity.h>
#include <qt/transactionrecord.h>
#include <qt/vaultmodel.h>

#include <QAbstractItemDelegate>
#include <QApplication>
#include <QColor>
#include <QFrame>
#include <QHBoxLayout>
#include <QDateTime>
#include <QLabel>
#include <QPainter>
#include <QPen>
#include <QPushButton>
#include <QStatusTipEvent>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>
#include <map>

#define DECORATION_SIZE 54
#define NUM_ITEMS 5

Q_DECLARE_METATYPE(interfaces::VaultBalances)

class TxViewDelegate : public QAbstractItemDelegate
{
    Q_OBJECT
public:
    explicit TxViewDelegate(const PlatformStyle* _platformStyle, QObject* parent = nullptr)
        : QAbstractItemDelegate(parent), platformStyle(_platformStyle)
    {
        connect(this, &TxViewDelegate::width_changed, this, &TxViewDelegate::sizeHintChanged);
    }

    inline void paint(QPainter *painter, const QStyleOptionViewItem &option,
                      const QModelIndex &index ) const override
    {
        painter->save();

        QIcon icon = qvariant_cast<QIcon>(index.data(TransactionTableModel::RawDecorationRole));
        QRect mainRect = option.rect.adjusted(0, 1, 0, -1);
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(QuicksilverStyle::Color(QuicksilverStyle::Token::CinnabarDim), 1));
        painter->setBrush(QuicksilverStyle::Color(QuicksilverStyle::Token::Rail));
        painter->drawRoundedRect(mainRect.adjusted(1, 1, -1, -1), 6, 6);

        QRect decorationRect(mainRect.topLeft() + QPoint(8, 4), QSize(DECORATION_SIZE - 8, DECORATION_SIZE - 8));
        int xspace = DECORATION_SIZE + 8;
        int ypad = 6;
        int halfheight = (mainRect.height() - 2*ypad)/2;
        QRect amountRect(mainRect.left() + xspace, mainRect.top()+ypad, mainRect.width() - xspace, halfheight);
        QRect addressRect(mainRect.left() + xspace, mainRect.top()+ypad+halfheight, mainRect.width() - xspace, halfheight);
        icon = platformStyle->SingleColorIcon(icon);
        icon.paint(painter, decorationRect);

        QDateTime date = index.data(TransactionTableModel::DateRole).toDateTime();
        QString address = index.data(Qt::DisplayRole).toString();
        qint64 amount = index.data(TransactionTableModel::AmountRole).toLongLong();
        bool confirmed = index.data(TransactionTableModel::ConfirmedRole).toBool();
        QVariant value = index.data(Qt::ForegroundRole);
        QColor foreground = option.palette.color(QPalette::Text);
        if(value.canConvert<QBrush>())
        {
            QBrush brush = qvariant_cast<QBrush>(value);
            foreground = brush.color();
        }

        painter->setPen(foreground);
        QRect boundingRect;
        painter->drawText(addressRect, Qt::AlignLeft | Qt::AlignVCenter, address, &boundingRect);

        if(amount < 0)
        {
            foreground = COLOR_NEGATIVE;
        }
        else if(!confirmed)
        {
            foreground = COLOR_UNCONFIRMED;
        }
        else
        {
            foreground = option.palette.color(QPalette::Text);
        }
        painter->setPen(foreground);
        QString amountText = QuicksilverUnits::formatWithUnit(unit, amount, true, QuicksilverUnits::SeparatorStyle::ALWAYS);
        if(!confirmed)
        {
            amountText = QString("[") + amountText + QString("]");
        }

        QRect amount_bounding_rect;
        painter->drawText(amountRect, Qt::AlignRight | Qt::AlignVCenter, amountText, &amount_bounding_rect);

        painter->setPen(option.palette.color(QPalette::Text));
        QRect date_bounding_rect;
        painter->drawText(amountRect, Qt::AlignLeft | Qt::AlignVCenter, GUIUtil::dateTimeStr(date), &date_bounding_rect);

        // 0.4*date_bounding_rect.width() is used to visually distinguish a date from an amount.
        const int minimum_width = 1.4 * date_bounding_rect.width() + amount_bounding_rect.width();
        const auto search = m_minimum_width.find(index.row());
        if (search == m_minimum_width.end() || search->second != minimum_width) {
            m_minimum_width[index.row()] = minimum_width;
            Q_EMIT width_changed(index);
        }

        painter->restore();
    }

    inline QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const auto search = m_minimum_width.find(index.row());
        const int minimum_text_width = search == m_minimum_width.end() ? 0 : search->second;
        return {DECORATION_SIZE + 8 + minimum_text_width, DECORATION_SIZE};
    }

    QuicksilverUnit unit{QuicksilverUnit::HG};

Q_SIGNALS:
    //! An intermediate signal for emitting from the `paint() const` member function.
    void width_changed(const QModelIndex& index) const;

private:
    const PlatformStyle* platformStyle;
    mutable std::map<int, int> m_minimum_width;
};

#include <qt/overviewpage.moc>

OverviewPage::OverviewPage(const PlatformStyle *platformStyle, QWidget *parent) :
    QWidget(parent),
    ui(new Ui::OverviewPage),
    m_platform_style{platformStyle},
    txdelegate(new TxViewDelegate(platformStyle, this))
{
    ui->setupUi(this);
    setProperty("class", QStringLiteral("quicksilverPage"));

    ui->frame->setObjectName(QStringLiteral("overviewBalancePanel"));
    ui->frame_2->setObjectName(QStringLiteral("overviewActivityPanel"));
    ui->label_5->setText(tr("Balance"));
    ui->label_5->setProperty("class", QStringLiteral("hudHeading"));
    ui->label_4->setText(tr("Ledger activity"));
    ui->label_4->setProperty("class", QStringLiteral("hudHeading"));
    ui->labelBalanceText->setText(tr("Confirmed"));
    ui->labelPendingText->setText(tr("Pending"));
    ui->labelImmatureText->setText(tr("Maturing"));
    ui->labelBalance->setProperty("class", QStringLiteral("hudValue"));
    ui->labelUnconfirmed->setProperty("class", QStringLiteral("hudValue"));
    ui->labelImmature->setProperty("class", QStringLiteral("hudValue"));
    ui->labelTotal->setProperty("class", QStringLiteral("hudValue"));

    auto* title = new QLabel(tr("Quicksilver HUD"), this);
    title->setObjectName(QStringLiteral("homeBootstrapTitle"));
    title->setProperty("class", QStringLiteral("pageTitle"));
    ui->topLayout->insertWidget(1, title);

    auto* subtitle = new QLabel(tr("Node and vault state."), this);
    subtitle->setObjectName(QStringLiteral("homeBootstrapSubtitle"));
    subtitle->setProperty("class", QStringLiteral("muted"));
    subtitle->setWordWrap(true);
    ui->topLayout->insertWidget(2, subtitle);

    m_backup_panel = new QFrame(this);
    m_backup_panel->setObjectName(QStringLiteral("overviewBackupPanel"));
    auto* backup_layout = new QVBoxLayout(m_backup_panel);
    backup_layout->setContentsMargins(16, 14, 16, 14);
    backup_layout->setSpacing(8);

    auto* backup_top = new QHBoxLayout();
    backup_top->setSpacing(10);
    auto* backup_title = new QLabel(tr("Vault backup"), m_backup_panel);
    backup_title->setObjectName(QStringLiteral("overviewBackupTitle"));
    backup_title->setProperty("class", QStringLiteral("hudHeading"));
    backup_top->addWidget(backup_title, 1);
    m_backup_state = new QLabel(m_backup_panel);
    m_backup_state->setObjectName(QStringLiteral("overviewBackupState"));
    m_backup_state->setProperty("class", QStringLiteral("launchCapabilityState"));
    backup_top->addWidget(m_backup_state);
    backup_layout->addLayout(backup_top);

    m_backup_summary = new QLabel(m_backup_panel);
    m_backup_summary->setObjectName(QStringLiteral("overviewBackupSummary"));
    m_backup_summary->setProperty("class", QStringLiteral("muted"));
    m_backup_summary->setWordWrap(true);
    backup_layout->addWidget(m_backup_summary);

    m_backup_button = new QPushButton(m_backup_panel);
    m_backup_button->setObjectName(QStringLiteral("overviewBackupButton"));
    connect(m_backup_button, &QPushButton::clicked, this, &OverviewPage::backupRequested);
    backup_layout->addWidget(m_backup_button, 0, Qt::AlignLeft);
    ui->topLayout->insertWidget(3, m_backup_panel);
    setBackupState(false);

    // use a SingleColorIcon for the "out of sync warning" icon
    QIcon icon = m_platform_style->ColorIcon(QStringLiteral(":/icons/warning"), QuicksilverStyle::Color(QuicksilverStyle::Token::Amber));
    ui->labelTransactionsStatus->setIcon(icon);
    ui->labelVaultStatus->setIcon(icon);

    // Recent ledger activity
    ui->listTransactions->setItemDelegate(txdelegate);
    ui->listTransactions->setIconSize(QSize(DECORATION_SIZE, DECORATION_SIZE));
    ui->listTransactions->setMinimumHeight(NUM_ITEMS * (DECORATION_SIZE + 2));
    ui->listTransactions->setAttribute(Qt::WA_MacShowFocusRect, false);

    connect(ui->listTransactions, &TransactionOverviewWidget::clicked, this, &OverviewPage::handleTransactionClicked);

    // start with displaying the "out of sync" warnings
    showOutOfSyncWarning(true);
    connect(ui->labelVaultStatus, &QPushButton::clicked, this, &OverviewPage::outOfSyncWarningClicked);
    connect(ui->labelTransactionsStatus, &QPushButton::clicked, this, &OverviewPage::outOfSyncWarningClicked);
}

void OverviewPage::handleTransactionClicked(const QModelIndex &index)
{
    if(filter)
        Q_EMIT transactionClicked(filter->mapToSource(index));
}

void OverviewPage::setPrivacy(bool privacy)
{
    m_privacy = privacy;
    // The client model only comes into existence once consensus has been enabled, but a
    // vault opens and runs without it, so reach the options model through the vault model
    // when consensus is off. Both models hold the application's single OptionsModel.
    if (OptionsModel* options_model = clientModel ? clientModel->getOptionsModel() : vaultModel->getOptionsModel()) {
        options_model->setOption(OptionsModel::OptionID::MaskValues, privacy);
    }
    const auto& balances = vaultModel->getCachedBalance();
    if (balances.balance != -1) {
        setBalance(balances);
    }

    ui->listTransactions->setVisible(!m_privacy);

    const QString status_tip = m_privacy ? tr("Privacy mode activated for the HUD. To unmask values, uncheck Controls->Mask values.") : "";
    setStatusTip(status_tip);
    QStatusTipEvent event(status_tip);
    QApplication::sendEvent(this, &event);
}

OverviewPage::~OverviewPage()
{
    delete ui;
}

void OverviewPage::setBalance(const interfaces::VaultBalances& balances)
{
    QuicksilverUnit unit = vaultModel->getOptionsModel()->getDisplayUnit();
    ui->labelBalance->setText(QuicksilverUnits::formatWithPrivacy(unit, balances.balance, QuicksilverUnits::SeparatorStyle::ALWAYS, m_privacy));
    ui->labelUnconfirmed->setText(QuicksilverUnits::formatWithPrivacy(unit, balances.unconfirmed_balance, QuicksilverUnits::SeparatorStyle::ALWAYS, m_privacy));
    ui->labelImmature->setText(QuicksilverUnits::formatWithPrivacy(unit, balances.immature_balance, QuicksilverUnits::SeparatorStyle::ALWAYS, m_privacy));
    ui->labelTotal->setText(QuicksilverUnits::formatWithPrivacy(unit, balances.balance + balances.unconfirmed_balance + balances.immature_balance + balances.delegated_balance, QuicksilverUnits::SeparatorStyle::ALWAYS, m_privacy));
    ui->labelDelegated->setText(QuicksilverUnits::formatWithPrivacy(unit, balances.delegated_balance, QuicksilverUnits::SeparatorStyle::ALWAYS, m_privacy));

    // only show immature (newly mined) balance if it's non-zero, so as not to complicate things
    // for the non-mining users
    bool showImmature = balances.immature_balance != 0;
    // Same reasoning for delegated: a vault that has never funded an agent should not have
    // to read a line explaining a concept it does not use.
    const bool showDelegated{balances.delegated_balance != 0};
    ui->labelDelegated->setVisible(showDelegated);
    ui->labelDelegatedText->setVisible(showDelegated);

    ui->labelImmature->setVisible(showImmature);
    ui->labelImmatureText->setVisible(showImmature);

    updateMaturityCountdown();
}

void OverviewPage::updateMaturityCountdown()
{
    // The soonest maturity is the honest answer to "when can I spend anything",
    // so take the minimum across every immature row rather than the last one.
    int soonest = 0;
    if (vaultModel && vaultModel->getTransactionTableModel()) {
        const TransactionTableModel* model = vaultModel->getTransactionTableModel();
        for (int row = 0; row < model->rowCount(QModelIndex()); ++row) {
            const QModelIndex idx = model->index(row, 0);
            const int status = idx.data(TransactionTableModel::StatusRole).toInt();
            if (status != TransactionStatus::Immature) continue;
            const int matures_in = idx.data(TransactionTableModel::MaturesInRole).toInt();
            if (matures_in > 0 && (soonest == 0 || matures_in < soonest)) soonest = matures_in;
        }
    }
    const QString text = qsmaturity::FormatMaturityCountdown(
        soonest, Params().GetConsensus().nPowTargetSpacing);
    ui->labelImmatureCountdown->setText(text);
    ui->labelImmatureCountdown->setVisible(!text.isEmpty());
}

void OverviewPage::setClientModel(ClientModel *model)
{
    this->clientModel = model;
    if (model) {
        // Show warning, for example if this is a prerelease version
        connect(model, &ClientModel::alertsChanged, this, &OverviewPage::updateAlerts);
        updateAlerts(model->getStatusBarWarnings());

        connect(model->getOptionsModel(), &OptionsModel::fontForMoneyChanged, this, &OverviewPage::setMonospacedFont);
        setMonospacedFont(clientModel->getOptionsModel()->getFontForMoney());
    }
}

void OverviewPage::setVaultModel(VaultModel *model)
{
    this->vaultModel = model;
    if(model && model->getOptionsModel())
    {
        // Set up transaction list
        filter.reset(new TransactionFilterProxy());
        filter->setSourceModel(model->getTransactionTableModel());
        filter->setDynamicSortFilter(true);
        filter->setSortRole(Qt::EditRole);
        filter->setShowInactive(false);
        filter->sort(TransactionTableModel::Date, Qt::DescendingOrder);

        ui->listTransactions->setModel(filter.get());
        ui->listTransactions->setModelColumn(TransactionTableModel::ToAddress);

        connect(filter.get(), &TransactionFilterProxy::rowsInserted, this, &OverviewPage::LimitTransactionRows);
        connect(filter.get(), &TransactionFilterProxy::rowsRemoved, this, &OverviewPage::LimitTransactionRows);
        connect(filter.get(), &TransactionFilterProxy::rowsMoved, this, &OverviewPage::LimitTransactionRows);
        LimitTransactionRows();
        // Keep up to date with vault
        setBalance(model->getCachedBalance());
        connect(model, &VaultModel::balanceChanged, this, &OverviewPage::setBalance);

        // The countdown is derived from transaction rows, not from the balance, so
        // it has to follow the transaction model as blocks arrive: without these it
        // would freeze at whatever it read when the balance last moved.
        connect(model->getTransactionTableModel(), &TransactionTableModel::dataChanged,
                this, &OverviewPage::updateMaturityCountdown);
        connect(model->getTransactionTableModel(), &TransactionTableModel::rowsInserted,
                this, &OverviewPage::updateMaturityCountdown);
        updateMaturityCountdown();

        connect(model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &OverviewPage::updateDisplayUnit);
    }

    // update the display unit, to not use the default ("Hg")
    updateDisplayUnit();
}

void OverviewPage::changeEvent(QEvent* e)
{
    if (e->type() == QEvent::PaletteChange) {
        QIcon icon = m_platform_style->ColorIcon(QStringLiteral(":/icons/warning"), QuicksilverStyle::Color(QuicksilverStyle::Token::Amber));
        ui->labelTransactionsStatus->setIcon(icon);
        ui->labelVaultStatus->setIcon(icon);
    }

    QWidget::changeEvent(e);
}

// Only show most recent NUM_ITEMS rows
void OverviewPage::LimitTransactionRows()
{
    if (filter && ui->listTransactions && ui->listTransactions->model() && filter.get() == ui->listTransactions->model()) {
        for (int i = 0; i < filter->rowCount(); ++i) {
            ui->listTransactions->setRowHidden(i, i >= NUM_ITEMS);
        }
    }
}

void OverviewPage::updateDisplayUnit()
{
    if (vaultModel && vaultModel->getOptionsModel()) {
        const auto& balances = vaultModel->getCachedBalance();
        if (balances.balance != -1) {
            setBalance(balances);
        }

        // Update txdelegate->unit with the current unit
        txdelegate->unit = vaultModel->getOptionsModel()->getDisplayUnit();

        ui->listTransactions->update();
    }
}

void OverviewPage::updateAlerts(const QString &warnings)
{
    this->ui->labelAlerts->setVisible(!warnings.isEmpty());
    this->ui->labelAlerts->setText(warnings);
}

void OverviewPage::showOutOfSyncWarning(bool fShow)
{
    ui->labelVaultStatus->setVisible(fShow);
    ui->labelTransactionsStatus->setVisible(fShow);
}

void OverviewPage::setBackupState(bool backup_done)
{
    m_backup_state->setText(backup_done ? tr("Done") : tr("Needed"));
    m_backup_summary->setText(backup_done
        ? tr("This vault records a completed backup. Make another backup after meaningful activity, because a backup only holds the history that existed when it was made.")
        : tr("Back up the vault file before relying on this desktop. This app does not issue a recovery phrase; only a current vault-file backup restores both spending keys and transaction history."));
    m_backup_button->setText(backup_done ? tr("Back up again") : tr("Back up vault"));
}

void OverviewPage::setMonospacedFont(const QFont& f)
{
    ui->labelBalance->setFont(f);
    ui->labelUnconfirmed->setFont(f);
    ui->labelImmature->setFont(f);
    ui->labelTotal->setFont(f);
}
