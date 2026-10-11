// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/transactionview.h>

#include <qt/addresstablemodel.h>
#include <qt/benchpanel.h>
#include <qt/quicksilverunits.h>
#include <qt/csvmodelwriter.h>
#include <qt/editaddressdialog.h>
#include <qt/guiutil.h>
#include <qt/ledgerrows.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/transactiondescdialog.h>
#include <qt/transactionfilterproxy.h>
#include <qt/transactionrecord.h>
#include <qt/transactiontablemodel.h>
#include <qt/vaultmodel.h>

#include <node/interface_ui.h>

#include <algorithm>
#include <chrono>
#include <optional>

#include <QApplication>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDesktopServices>
#include <QDoubleValidator>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPoint>
#include <QScrollBar>
#include <QSettings>
#include <QPointer>
#include <QPushButton>
#include <QTableView>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

TransactionView::TransactionView(const PlatformStyle *platformStyle, QWidget *parent)
    : QWidget(parent), m_platform_style{platformStyle}
{
    // Build filter row
    setContentsMargins(14, 14, 14, 14);

    QHBoxLayout *hlayout = new QHBoxLayout();
    hlayout->setSpacing(8);

    // Each filter's first entry names what it filters: two combos both reading
    // "All" said nothing about which was which.
    dateWidget = new QComboBox(this);
    dateWidget->setAccessibleName(tr("Filter by date"));
    dateWidget->addItem(tr("All dates"), All);
    dateWidget->addItem(tr("Today"), Today);
    dateWidget->addItem(tr("This week"), ThisWeek);
    dateWidget->addItem(tr("This month"), ThisMonth);
    dateWidget->addItem(tr("Last month"), LastMonth);
    dateWidget->addItem(tr("This year"), ThisYear);
    dateWidget->addItem(tr("Range…"), Range);
    hlayout->addWidget(dateWidget);

    typeWidget = new QComboBox(this);
    typeWidget->setAccessibleName(tr("Filter by type"));
    typeWidget->addItem(tr("All types"), TransactionFilterProxy::ALL_TYPES);
    typeWidget->addItem(TransactionTableModel::typeWord(TransactionRecord::RecvWithAddress), TransactionFilterProxy::TYPE(TransactionRecord::RecvWithAddress) |
                                        TransactionFilterProxy::TYPE(TransactionRecord::RecvFromOther));
    typeWidget->addItem(TransactionTableModel::typeWord(TransactionRecord::SendToAddress), TransactionFilterProxy::TYPE(TransactionRecord::SendToAddress) |
                                  TransactionFilterProxy::TYPE(TransactionRecord::SendToOther));
    typeWidget->addItem(TransactionTableModel::typeWord(TransactionRecord::Generated), TransactionFilterProxy::TYPE(TransactionRecord::Generated));
    typeWidget->addItem(tr("Other"), TransactionFilterProxy::TYPE(TransactionRecord::Other));

    hlayout->addWidget(typeWidget);

    search_widget = new QLineEdit(this);
    search_widget->setPlaceholderText(tr("Enter address, transaction id, or label to search"));
    hlayout->addWidget(search_widget, 1);

    amountWidget = new QLineEdit(this);
    amountWidget->setPlaceholderText(tr("Min amount"));
    amountWidget->setFixedWidth(110);
    QDoubleValidator *amountValidator = new QDoubleValidator(0, 1e20, 8, this);
    QLocale amountLocale(QLocale::C);
    amountLocale.setNumberOptions(QLocale::RejectGroupSeparator);
    amountValidator->setLocale(amountLocale);
    amountWidget->setValidator(amountValidator);
    hlayout->addWidget(amountWidget);

    // Delay before filtering transactions
    static constexpr auto input_filter_delay{200ms};

    QTimer* amount_typing_delay = new QTimer(this);
    amount_typing_delay->setSingleShot(true);
    amount_typing_delay->setInterval(input_filter_delay);

    QTimer* prefix_typing_delay = new QTimer(this);
    prefix_typing_delay->setSingleShot(true);
    prefix_typing_delay->setInterval(input_filter_delay);

    QVBoxLayout *vlayout = new QVBoxLayout(this);
    vlayout->setContentsMargins(0,0,0,0);
    vlayout->setSpacing(0);

    // The ledger is one Bench panel: filters over the table.
    const BenchPanel::Parts panel = BenchPanel::Make(QStringLiteral("transactionLedgerPanel"), tr("Ledger"), this);
    panel.frame->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    panel.body->setContentsMargins(0, 0, 0, 0);
    panel.body->setSpacing(0);
    hlayout->setContentsMargins(12, 10, 12, 10);
    transactionView = new QTableView(this);
    transactionView->setObjectName("transactionView");
    transactionView->setProperty("class", QStringLiteral("benchTable"));
    transactionView->setFrameShape(QFrame::NoFrame);
    panel.body->addLayout(hlayout);
    panel.body->addWidget(createDateRangeWidget());
    panel.body->addWidget(transactionView, 1);
    // What a bracketed amount and the clock mean, said once on the page.
    auto* legend = new QLabel(tr("Pending and Maturing amounts are not spendable yet: a pending transfer is waiting for a block, and a maturing mining reward is still maturing. Point at a row's state for its details."), panel.frame);
    legend->setObjectName(QStringLiteral("transactionLedgerLegend"));
    legend->setProperty("class", QStringLiteral("benchNote"));
    legend->setWordWrap(true);
    legend->setContentsMargins(12, 8, 12, 10);
    panel.body->addWidget(legend);
    // Export is one of the panel's own commands, in its head.
    auto* export_button = new QPushButton(tr("&Export"), panel.frame);
    export_button->setObjectName(QStringLiteral("transactionExportButton"));
    export_button->setProperty("class", QStringLiteral("benchQuiet"));
    export_button->setToolTip(tr("Export the ledger to a file"));
    panel.head->addWidget(export_button);
    connect(export_button, &QPushButton::clicked, this, &TransactionView::exportClicked);
    vlayout->addWidget(panel.frame);
    transactionView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    transactionView->setTabKeyNavigation(false);
    transactionView->setContextMenuPolicy(Qt::CustomContextMenu);
    transactionView->installEventFilter(this);
    transactionView->setAlternatingRowColors(true);
    transactionView->setSelectionBehavior(QAbstractItemView::SelectRows);
    transactionView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    transactionView->setSortingEnabled(true);
    transactionView->verticalHeader()->hide();

    QSettings settings;
    if (!transactionView->horizontalHeader()->restoreState(settings.value("TransactionViewHeaderState").toByteArray())) {
        transactionView->setColumnWidth(TransactionTableModel::Status, STATUS_COLUMN_WIDTH);
        transactionView->setColumnWidth(TransactionTableModel::Date, DATE_COLUMN_WIDTH);
        transactionView->setColumnWidth(TransactionTableModel::Type, TYPE_COLUMN_WIDTH);
        transactionView->setColumnWidth(TransactionTableModel::Amount, AMOUNT_MINIMUM_COLUMN_WIDTH);
        transactionView->horizontalHeader()->setMinimumSectionSize(MINIMUM_COLUMN_WIDTH);
    }
    // Label takes the room left over, whatever a saved header state said: with
    // the last section stretching, the room went to a gap before Amount while
    // dates and labels were cut short.
    transactionView->horizontalHeader()->setStretchLastSection(false);

    contextMenu = new QMenu(this);
    contextMenu->setObjectName("contextMenu");
    copyAddressAction = contextMenu->addAction(tr("&Copy address"), this, &TransactionView::copyAddress);
    copyLabelAction = contextMenu->addAction(tr("Copy &label"), this, &TransactionView::copyLabel);
    contextMenu->addAction(tr("Copy &amount"), this, &TransactionView::copyAmount);
    contextMenu->addAction(tr("Copy transaction &ID"), this, &TransactionView::copyTxID);
    contextMenu->addAction(tr("Copy &raw transaction"), this, &TransactionView::copyTxHex);
    contextMenu->addAction(tr("Copy full transaction &details"), this, &TransactionView::copyTxPlainText);
    contextMenu->addAction(tr("&Show transaction details"), this, &TransactionView::showDetails);
    contextMenu->addSeparator();
    abandonAction = contextMenu->addAction(tr("A&bandon transaction"), this, &TransactionView::abandonTx);
    contextMenu->addAction(tr("&Edit address label"), this, &TransactionView::editLabel);

    connect(dateWidget, qOverload<int>(&QComboBox::activated), this, &TransactionView::chooseDate);
    connect(typeWidget, qOverload<int>(&QComboBox::activated), this, &TransactionView::chooseType);
    connect(amountWidget, &QLineEdit::textChanged, amount_typing_delay, qOverload<>(&QTimer::start));
    connect(amount_typing_delay, &QTimer::timeout, this, &TransactionView::changedAmount);
    connect(search_widget, &QLineEdit::textChanged, prefix_typing_delay, qOverload<>(&QTimer::start));
    connect(prefix_typing_delay, &QTimer::timeout, this, &TransactionView::changedSearch);

    connect(transactionView, &QTableView::doubleClicked, this, &TransactionView::doubleClicked);
    connect(transactionView, &QTableView::customContextMenuRequested, this, &TransactionView::contextualMenu);

    // Double-clicking on a transaction on the transaction history page shows details
    connect(this, &TransactionView::doubleClicked, this, &TransactionView::showDetails);
}

TransactionView::~TransactionView()
{
    QSettings settings;
    settings.setValue("TransactionViewHeaderState", transactionView->horizontalHeader()->saveState());
}

void TransactionView::setModel(VaultModel *_model)
{
    if (transactionProxyModel) {
        transactionView->setModel(nullptr);
        delete m_ledger_rows;
        m_ledger_rows = nullptr;
        delete transactionProxyModel;
        transactionProxyModel = nullptr;
    }
    this->model = _model;
    if(_model)
    {
        transactionProxyModel = new TransactionFilterProxy(this);
        transactionProxyModel->setSourceModel(_model->getTransactionTableModel());
        transactionProxyModel->setDynamicSortFilter(true);
        transactionProxyModel->setSortCaseSensitivity(Qt::CaseInsensitive);
        transactionProxyModel->setFilterCaseSensitivity(Qt::CaseInsensitive);
        transactionProxyModel->setSortRole(Qt::EditRole);
        // The rows read as on Home: state word, friendly date with the state
        // dot, label, signed amount. Sorting and filtering stay underneath.
        m_ledger_rows = new LedgerRows(LedgerRows::Tooltips::All, this);
        m_ledger_rows->setSourceModel(transactionProxyModel);
        if (OptionsModel* options = _model->getOptionsModel()) {
            m_ledger_rows->setDisplay(options->getDisplayUnit(), /*privacy=*/false);
            connect(options, &OptionsModel::displayUnitChanged, m_ledger_rows, [this, options] {
                m_ledger_rows->setDisplay(options->getDisplayUnit(), /*privacy=*/false);
            });
        }
        transactionView->setModel(m_ledger_rows);
        transactionView->sortByColumn(TransactionTableModel::Date, Qt::DescendingOrder);
        transactionView->horizontalHeader()->setSectionResizeMode(TransactionTableModel::ToAddress, QHeaderView::Stretch);
        for (const auto& signal : {&QAbstractItemModel::rowsInserted, &QAbstractItemModel::rowsRemoved}) {
            connect(m_ledger_rows, signal, this, &TransactionView::fitColumns);
        }
        connect(m_ledger_rows, &QAbstractItemModel::modelReset, this, &TransactionView::fitColumns);
        connect(m_ledger_rows, &QAbstractItemModel::dataChanged, this, &TransactionView::fitColumns);
        fitColumns();

        if (_model->getOptionsModel() && !m_third_party_actions)
        {
            m_third_party_actions = true;
            // Add third party transaction URLs to context menu
            QStringList listUrls = GUIUtil::SplitSkipEmptyParts(_model->getOptionsModel()->getThirdPartyTxUrls(), "|");
            bool actions_created = false;
            for (int i = 0; i < listUrls.size(); ++i)
            {
                QString url = listUrls[i].trimmed();
                QString host = QUrl(url, QUrl::StrictMode).host();
                if (!host.isEmpty())
                {
                    if (!actions_created) {
                        contextMenu->addSeparator();
                        actions_created = true;
                    }
                    /*: Transactions table context menu action to show the
                        selected transaction in a third-party block explorer.
                        %1 is a stand-in argument for the URL of the explorer. */
                    contextMenu->addAction(tr("Show in %1").arg(host), [this, url] { openThirdPartyTxUrl(url); });
                }
            }
        }
    }
}

void TransactionView::fitColumns()
{
    QHeaderView* header = transactionView->horizontalHeader();
    if (header->count() <= TransactionTableModel::Amount) return;
    // QTableView narrows the public QAbstractItemView call to protected.
    QAbstractItemView* cells = transactionView;
    const auto content = [&](int column) { return std::max(cells->sizeHintForColumn(column), header->sectionSizeHint(column)); };
    // State, Date and Type keep a wider width the user chose; Amount is exactly as
    // wide as its figures, and Label stretches over the rest.
    for (int column : {TransactionTableModel::Status, TransactionTableModel::Date, TransactionTableModel::Type}) {
        if (transactionView->columnWidth(column) < content(column)) transactionView->setColumnWidth(column, content(column));
    }
    transactionView->setColumnWidth(TransactionTableModel::Amount, content(TransactionTableModel::Amount));
}

void TransactionView::changeEvent(QEvent* e)
{
    if (e->type() == QEvent::PaletteChange) {
    }

    QWidget::changeEvent(e);
}

void TransactionView::chooseDate(int idx)
{
    if (!transactionProxyModel) return;
    QDate current = QDate::currentDate();
    dateRangeWidget->setVisible(false);
    switch(dateWidget->itemData(idx).toInt())
    {
    case All:
        transactionProxyModel->setDateRange(
                std::nullopt,
                std::nullopt);
        break;
    case Today:
        transactionProxyModel->setDateRange(
                GUIUtil::StartOfDay(current),
                std::nullopt);
        break;
    case ThisWeek: {
        // Find last Monday
        QDate startOfWeek = current.addDays(-(current.dayOfWeek()-1));
        transactionProxyModel->setDateRange(
                GUIUtil::StartOfDay(startOfWeek),
                std::nullopt);

        } break;
    case ThisMonth:
        transactionProxyModel->setDateRange(
                GUIUtil::StartOfDay(QDate(current.year(), current.month(), 1)),
                std::nullopt);
        break;
    case LastMonth:
        transactionProxyModel->setDateRange(
                GUIUtil::StartOfDay(QDate(current.year(), current.month(), 1).addMonths(-1)),
                GUIUtil::StartOfDay(QDate(current.year(), current.month(), 1)));
        break;
    case ThisYear:
        transactionProxyModel->setDateRange(
                GUIUtil::StartOfDay(QDate(current.year(), 1, 1)),
                std::nullopt);
        break;
    case Range:
        dateRangeWidget->setVisible(true);
        dateRangeChanged();
        break;
    }
}

void TransactionView::chooseType(int idx)
{
    if(!transactionProxyModel)
        return;
    transactionProxyModel->setTypeFilter(
        typeWidget->itemData(idx).toInt());
}

void TransactionView::changedSearch()
{
    if(!transactionProxyModel)
        return;
    transactionProxyModel->setSearchString(search_widget->text());
}

void TransactionView::changedAmount()
{
    if(!transactionProxyModel)
        return;
    CAmount amount_parsed = 0;
    if (QuicksilverUnits::parse(model->getOptionsModel()->getDisplayUnit(), amountWidget->text(), &amount_parsed)) {
        transactionProxyModel->setMinAmount(amount_parsed);
    }
    else
    {
        transactionProxyModel->setMinAmount(0);
    }
}

void TransactionView::exportClicked()
{
    if (!model || !model->getOptionsModel()) {
        return;
    }

    QPointer<TransactionView> self{this};
    GUIUtil::getSaveFileName(this,
        tr("Export Transaction History"), QString(),
        /*: Expanded name of the CSV file format.
            See: https://en.wikipedia.org/wiki/Comma-separated_values. */
        tr("Comma separated file") + QLatin1String(" (*.csv)"),
        [self](const QString& filename) {
            if (!self || filename.isEmpty() || !self->model || !self->model->getOptionsModel()) return;

            CSVModelWriter writer(filename);
            writer.setModel(self->transactionProxyModel);
            writer.addColumn(self->tr("Confirmed"), 0, TransactionTableModel::ConfirmedRole);
            writer.addColumn(self->tr("Date"), 0, TransactionTableModel::DateRole);
            writer.addColumn(self->tr("Type"), TransactionTableModel::Type, Qt::EditRole);
            writer.addColumn(self->tr("Label"), 0, TransactionTableModel::LabelRole);
            writer.addColumn(self->tr("Address"), 0, TransactionTableModel::AddressRole);
            writer.addColumn(QuicksilverUnits::getAmountColumnTitle(self->model->getOptionsModel()->getDisplayUnit()), 0, TransactionTableModel::FormattedAmountRole);
            writer.addColumn(self->tr("ID"), 0, TransactionTableModel::TxHashRole);

            if(!writer.write()) {
                Q_EMIT self->message(self->tr("Exporting Failed"), self->tr("There was an error trying to save the transaction history to %1.").arg(filename),
                    CClientUIInterface::MSG_ERROR);
            } else {
                Q_EMIT self->message(self->tr("Exporting Successful"), self->tr("The transaction history was successfully saved to %1.").arg(filename),
                    CClientUIInterface::MSG_INFORMATION);
            }
        });
}

void TransactionView::contextualMenu(const QPoint &point)
{
    QModelIndex index = transactionView->indexAt(point);
    QModelIndexList selection = transactionView->selectionModel()->selectedRows(0);
    if (selection.empty())
        return;

    // check if transaction can be abandoned, disable context menu action in case it doesn't
    uint256 hash;
    hash.SetHexDeprecated(selection.at(0).data(TransactionTableModel::TxHashRole).toString().toStdString());
    abandonAction->setEnabled(model->vault().transactionCanBeAbandoned(hash));
    copyAddressAction->setEnabled(GUIUtil::hasEntryData(transactionView, 0, TransactionTableModel::AddressRole));
    copyLabelAction->setEnabled(GUIUtil::hasEntryData(transactionView, 0, TransactionTableModel::LabelRole));

    if (index.isValid()) {
        GUIUtil::PopupMenu(contextMenu, transactionView->viewport()->mapToGlobal(point));
    }
}

void TransactionView::abandonTx()
{
    if(!transactionView || !transactionView->selectionModel())
        return;
    QModelIndexList selection = transactionView->selectionModel()->selectedRows(0);

    // get the hash from the TxHashRole (QVariant / QString)
    uint256 hash;
    QString hashQStr = selection.at(0).data(TransactionTableModel::TxHashRole).toString();
    hash.SetHexDeprecated(hashQStr.toStdString());

    // Abandon the vault transaction over the vaultModel
    model->vault().abandonTransaction(hash);
}

void TransactionView::copyAddress()
{
    GUIUtil::copyEntryData(transactionView, 0, TransactionTableModel::AddressRole);
}

void TransactionView::copyLabel()
{
    GUIUtil::copyEntryData(transactionView, 0, TransactionTableModel::LabelRole);
}

void TransactionView::copyAmount()
{
    GUIUtil::copyEntryData(transactionView, 0, TransactionTableModel::FormattedAmountRole);
}

void TransactionView::copyTxID()
{
    GUIUtil::copyEntryData(transactionView, 0, TransactionTableModel::TxHashRole);
}

void TransactionView::copyTxHex()
{
    GUIUtil::copyEntryData(transactionView, 0, TransactionTableModel::TxHexRole);
}

void TransactionView::copyTxPlainText()
{
    GUIUtil::copyEntryData(transactionView, 0, TransactionTableModel::TxPlainTextRole);
}

void TransactionView::editLabel()
{
    if(!transactionView->selectionModel() ||!model)
        return;
    QModelIndexList selection = transactionView->selectionModel()->selectedRows();
    if(!selection.isEmpty())
    {
        AddressTableModel *addressBook = model->getAddressTableModel();
        if(!addressBook)
            return;
        QString address = selection.at(0).data(TransactionTableModel::AddressRole).toString();
        if(address.isEmpty())
        {
            // If this transaction has no associated address, exit
            return;
        }
        // Is address in address book? Address book can miss address when a transaction is
        // sent from outside the UI.
        int idx = addressBook->lookupAddress(address);
        if(idx != -1)
        {
            // Edit sending / receiving address
            QModelIndex modelIdx = addressBook->index(idx, 0, QModelIndex());
            // Determine type of address, launch appropriate editor dialog type
            QString type = modelIdx.data(AddressTableModel::TypeRole).toString();

            auto dlg = new EditAddressDialog(
                type == AddressTableModel::Receive
                ? EditAddressDialog::EditReceivingAddress
                : EditAddressDialog::EditSendingAddress, this);
            dlg->setModel(addressBook);
            dlg->loadRow(idx);
            GUIUtil::ShowModalDialogAsynchronously(dlg);
        }
        else
        {
            // Add sending address
            auto dlg = new EditAddressDialog(EditAddressDialog::NewSendingAddress,
                this);
            dlg->setModel(addressBook);
            dlg->setAddress(address);
            GUIUtil::ShowModalDialogAsynchronously(dlg);
        }
    }
}

void TransactionView::showDetails()
{
    if(!transactionView->selectionModel())
        return;
    QModelIndexList selection = transactionView->selectionModel()->selectedRows();
    if(!selection.isEmpty())
    {
        TransactionDescDialog *dlg = new TransactionDescDialog(selection.at(0));
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        m_opened_dialogs.append(dlg);
        connect(dlg, &QObject::destroyed, [this, dlg] {
            m_opened_dialogs.removeOne(dlg);
        });
        dlg->show();
    }
}

void TransactionView::openThirdPartyTxUrl(QString url)
{
    if(!transactionView || !transactionView->selectionModel())
        return;
    QModelIndexList selection = transactionView->selectionModel()->selectedRows(0);
    if(!selection.isEmpty())
         QDesktopServices::openUrl(QUrl::fromUserInput(url.replace("%s", selection.at(0).data(TransactionTableModel::TxHashRole).toString())));
}

QWidget *TransactionView::createDateRangeWidget()
{
    dateRangeWidget = new QFrame();
    dateRangeWidget->setFrameStyle(static_cast<int>(QFrame::Panel) | static_cast<int>(QFrame::Raised));
    dateRangeWidget->setContentsMargins(1,1,1,1);
    QHBoxLayout *layout = new QHBoxLayout(dateRangeWidget);
    layout->setContentsMargins(0,0,0,0);
    layout->addSpacing(23);
    layout->addWidget(new QLabel(tr("Range:")));

    dateFrom = new QDateTimeEdit(this);
    dateFrom->setDisplayFormat("dd/MM/yy");
    dateFrom->setCalendarPopup(true);
    dateFrom->setMinimumWidth(100);
    dateFrom->setDate(QDate::currentDate().addDays(-7));
    layout->addWidget(dateFrom);
    layout->addWidget(new QLabel(tr("to")));

    dateTo = new QDateTimeEdit(this);
    dateTo->setDisplayFormat("dd/MM/yy");
    dateTo->setCalendarPopup(true);
    dateTo->setMinimumWidth(100);
    dateTo->setDate(QDate::currentDate());
    layout->addWidget(dateTo);
    layout->addStretch();

    // Hide by default
    dateRangeWidget->setVisible(false);

    // Notify on change
    connect(dateFrom, &QDateTimeEdit::dateChanged, this, &TransactionView::dateRangeChanged);
    connect(dateTo, &QDateTimeEdit::dateChanged, this, &TransactionView::dateRangeChanged);

    return dateRangeWidget;
}

void TransactionView::dateRangeChanged()
{
    if(!transactionProxyModel)
        return;
    transactionProxyModel->setDateRange(
            GUIUtil::StartOfDay(dateFrom->date()),
            GUIUtil::StartOfDay(dateTo->date()).addDays(1));
}

void TransactionView::focusTransaction(const QModelIndex &idx)
{
    if(!transactionProxyModel)
        return;
    QModelIndex targetIdx = m_ledger_rows->mapFromSource(transactionProxyModel->mapFromSource(idx));
    transactionView->scrollTo(targetIdx);
    transactionView->setCurrentIndex(targetIdx);
    transactionView->setFocus();
}

void TransactionView::focusTransaction(const uint256& txid)
{
    if (!transactionProxyModel)
        return;

    const QModelIndexList results = this->model->getTransactionTableModel()->match(
        this->model->getTransactionTableModel()->index(0,0),
        TransactionTableModel::TxHashRole,
        QString::fromStdString(txid.ToString()), -1);

    transactionView->setFocus();
    transactionView->selectionModel()->clearSelection();
    for (const QModelIndex& index : results) {
        const QModelIndex targetIndex = m_ledger_rows->mapFromSource(transactionProxyModel->mapFromSource(index));
        transactionView->selectionModel()->select(
            targetIndex,
            QItemSelectionModel::Rows | QItemSelectionModel::Select);
        // Called once per destination to ensure all results are in view, unless
        // transactions are not ordered by (ascending or descending) date.
        transactionView->scrollTo(targetIndex);
        // scrollTo() does not scroll far enough the first time when transactions
        // are ordered by ascending date.
        if (index == results[0]) transactionView->scrollTo(targetIndex);
    }
}

// Need to override default Ctrl+C action for amount as default behaviour is just to copy DisplayRole text
bool TransactionView::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::KeyPress)
    {
        QKeyEvent *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_C && ke->modifiers().testFlag(Qt::ControlModifier))
        {
             GUIUtil::copyEntryData(transactionView, 0, TransactionTableModel::TxPlainTextRole);
             return true;
        }
    }
    if (event->type() == QEvent::EnabledChange) {
        if (!isEnabled()) {
            closeOpenedDialogs();
        }
    }
    return QWidget::eventFilter(obj, event);
}

void TransactionView::closeOpenedDialogs()
{
    // close all dialogs opened from this view
    for (QDialog* dlg : m_opened_dialogs) {
        dlg->close();
    }
    m_opened_dialogs.clear();
}
