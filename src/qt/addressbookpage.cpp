// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/addressbookpage.h>
#include <qt/forms/ui_addressbookpage.h>

#include <qt/addresstablemodel.h>
#include <qt/csvmodelwriter.h>
#include <qt/editaddressdialog.h>
#include <qt/guiutil.h>
#include <qt/platformstyle.h>
#include <qt/quicksilverstyle.h>

#include <QIcon>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QSortFilterProxyModel>
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
#include <QRegularExpression>
#else
#include <QRegExp>
#endif

class AddressBookSortFilterProxyModel final : public QSortFilterProxyModel
{
    const QString m_type;

public:
    AddressBookSortFilterProxyModel(const QString& type, QObject* parent)
        : QSortFilterProxyModel(parent)
        , m_type(type)
    {
        setDynamicSortFilter(true);
        setFilterCaseSensitivity(Qt::CaseInsensitive);
        setSortCaseSensitivity(Qt::CaseInsensitive);
    }

protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override
    {
        auto model = sourceModel();
        auto label = model->index(row, AddressTableModel::Label, parent);

        if (model->data(label, AddressTableModel::TypeRole).toString() != m_type) {
            return false;
        }

        auto address = model->index(row, AddressTableModel::Address, parent);

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
        const auto pattern = filterRegularExpression();
#else
        const auto pattern = filterRegExp();
#endif
        return (model->data(address).toString().contains(pattern) ||
                model->data(label).toString().contains(pattern));
    }
};

AddressBookPage::AddressBookPage(const PlatformStyle *platformStyle, Mode _mode, Tabs _tab, QWidget *parent) :
    QDialog(parent, GUIUtil::dialog_flags),
    ui(new Ui::AddressBookPage),
    mode(_mode),
    tab(_tab)
{
    ui->setupUi(this);

    if (!platformStyle->getImagesOnButtons()) {
        ui->newAddress->setIcon(QIcon());
        ui->copyAddress->setIcon(QIcon());
        ui->deleteAddress->setIcon(QIcon());
        ui->exportButton->setIcon(QIcon());
    } else {
        ui->newAddress->setIcon(platformStyle->ColorIcon(":/icons/add", QuicksilverStyle::Color(QuicksilverStyle::Token::Teal)));
        ui->copyAddress->setIcon(platformStyle->ColorIcon(":/icons/editcopy", QuicksilverStyle::Color(QuicksilverStyle::Token::SilverMuted)));
        ui->deleteAddress->setIcon(platformStyle->ColorIcon(":/icons/remove", QuicksilverStyle::Color(QuicksilverStyle::Token::CinnabarBright)));
        ui->exportButton->setIcon(platformStyle->ColorIcon(":/icons/export", QuicksilverStyle::Color(QuicksilverStyle::Token::Amber)));
    }

    if (mode == ForSelection) {
        switch(tab)
        {
        case SendingTab: setWindowTitle(tr("Choose a transfer destination address")); break;
        case ReceivingTab: setWindowTitle(tr("Choose a receiving address")); break;
        }
        connect(ui->tableView, &QTableView::doubleClicked, this, &QDialog::accept);
        ui->tableView->setEditTriggers(QAbstractItemView::NoEditTriggers);
        ui->tableView->setFocus();
        ui->closeButton->setText(tr("C&hoose"));
        ui->exportButton->hide();
    }
    switch(tab)
    {
    case SendingTab:
        ui->labelExplanation->setText(tr("These are your saved Quicksilver transfer destinations. Always check the amount and receiving address before transmitting."));
        ui->deleteAddress->setVisible(true);
        ui->newAddress->setVisible(true);
        break;
    case ReceivingTab:
        ui->labelExplanation->setText(tr("These are your Quicksilver receiving addresses. Use the 'Generate receiving address' button in the request tab to create new addresses.\nMessage signing is only possible with Base58 addresses."));
        ui->deleteAddress->setVisible(false);
        ui->newAddress->setVisible(false);
        break;
    }

    // Build context menu
    contextMenu = new QMenu(this);
    contextMenu->addAction(tr("&Copy Address"), this, &AddressBookPage::on_copyAddress_clicked);
    contextMenu->addAction(tr("Copy &Label"), this, &AddressBookPage::onCopyLabelAction);
    contextMenu->addAction(tr("&Edit"), this, &AddressBookPage::onEditAction);

    if (tab == SendingTab) {
        contextMenu->addAction(tr("&Delete"), this, &AddressBookPage::on_deleteAddress_clicked);
    }

    connect(ui->tableView, &QWidget::customContextMenuRequested, this, &AddressBookPage::contextualMenu);
    connect(ui->closeButton, &QPushButton::clicked, this, &QDialog::accept);

    GUIUtil::handleCloseWindowShortcut(this);
}

AddressBookPage::~AddressBookPage()
{
    delete ui;
}

void AddressBookPage::setModel(AddressTableModel *_model)
{
    this->model = _model;
    if(!_model)
        return;

    auto type = tab == ReceivingTab ? AddressTableModel::Receive : AddressTableModel::Send;
    proxyModel = new AddressBookSortFilterProxyModel(type, this);
    proxyModel->setSourceModel(_model);

    connect(ui->searchLineEdit, &QLineEdit::textChanged, proxyModel, &QSortFilterProxyModel::setFilterWildcard);

    ui->tableView->setModel(proxyModel);
    ui->tableView->sortByColumn(0, Qt::AscendingOrder);

    // Set column widths
    ui->tableView->horizontalHeader()->setSectionResizeMode(AddressTableModel::Label, QHeaderView::Stretch);
    ui->tableView->horizontalHeader()->setSectionResizeMode(AddressTableModel::Address, QHeaderView::ResizeToContents);

    connect(ui->tableView->selectionModel(), &QItemSelectionModel::selectionChanged,
        this, &AddressBookPage::selectionChanged);

    // Select row for newly created address
    connect(_model, &AddressTableModel::rowsInserted, this, &AddressBookPage::selectNewAddress);

    selectionChanged();
    this->updateWindowsTitleWithVaultName();
}

void AddressBookPage::on_copyAddress_clicked()
{
    GUIUtil::copyEntryData(ui->tableView, AddressTableModel::Address);
}

void AddressBookPage::onCopyLabelAction()
{
    GUIUtil::copyEntryData(ui->tableView, AddressTableModel::Label);
}

void AddressBookPage::onEditAction()
{
    if(!model)
        return;

    if(!ui->tableView->selectionModel())
        return;
    QModelIndexList indexes = ui->tableView->selectionModel()->selectedRows();
    if(indexes.isEmpty())
        return;

    auto dlg = new EditAddressDialog(
        tab == SendingTab ?
        EditAddressDialog::EditSendingAddress :
        EditAddressDialog::EditReceivingAddress, this);
    dlg->setModel(model);
    QModelIndex origIndex = proxyModel->mapToSource(indexes.at(0));
    dlg->loadRow(origIndex.row());
    GUIUtil::ShowModalDialogAsynchronously(dlg);
}

void AddressBookPage::on_newAddress_clicked()
{
    if(!model)
        return;

    if (tab == ReceivingTab) {
        return;
    }

    auto dlg = new EditAddressDialog(EditAddressDialog::NewSendingAddress, this);
    dlg->setModel(model);
    connect(dlg, &QDialog::accepted, this, [this, dlg] {
        newAddressToSelect = dlg->getAddress();
    });
    GUIUtil::ShowModalDialogAsynchronously(dlg);
}

void AddressBookPage::on_deleteAddress_clicked()
{
    QTableView *table = ui->tableView;
    if(!table->selectionModel())
        return;

    QModelIndexList indexes = table->selectionModel()->selectedRows();
    if(!indexes.isEmpty())
    {
        table->model()->removeRow(indexes.at(0).row());
    }
}

void AddressBookPage::selectionChanged()
{
    // Set button states based on selected tab and selection
    QTableView *table = ui->tableView;
    if(!table->selectionModel())
        return;

    if(table->selectionModel()->hasSelection())
    {
        switch(tab)
        {
        case SendingTab:
            // In sending tab, allow deletion of selection
            ui->deleteAddress->setEnabled(true);
            ui->deleteAddress->setVisible(true);
            break;
        case ReceivingTab:
            // Deleting receiving addresses, however, is not allowed
            ui->deleteAddress->setEnabled(false);
            ui->deleteAddress->setVisible(false);
            break;
        }
        ui->copyAddress->setEnabled(true);
    }
    else
    {
        ui->deleteAddress->setEnabled(false);
        ui->copyAddress->setEnabled(false);
    }
}

void AddressBookPage::done(int retval)
{
    QTableView *table = ui->tableView;
    if(!table->selectionModel() || !table->model())
        return;

    // Figure out which address was selected, and return it
    QModelIndexList indexes = table->selectionModel()->selectedRows(AddressTableModel::Address);

    for (const QModelIndex& index : indexes) {
        QVariant address = table->model()->data(index);
        returnValue = address.toString();
    }

    if(returnValue.isEmpty())
    {
        // If no address entry selected, return rejected
        retval = Rejected;
    }

    QDialog::done(retval);
}

void AddressBookPage::on_exportButton_clicked()
{
    QPointer<AddressBookPage> self{this};
    GUIUtil::getSaveFileName(this,
        tr("Export Address List"), QString(),
        /*: Expanded name of the CSV file format.
            See: https://en.wikipedia.org/wiki/Comma-separated_values. */
        tr("Comma separated file") + QLatin1String(" (*.csv)"),
        [self](const QString& filename) {
            if (!self || filename.isEmpty()) return;

            CSVModelWriter writer(filename);
            writer.setModel(self->proxyModel);
            writer.addColumn("Label", AddressTableModel::Label, Qt::EditRole);
            writer.addColumn("Address", AddressTableModel::Address, Qt::EditRole);

            if(!writer.write()) {
                auto* box = new QMessageBox(QMessageBox::Critical, self->tr("Exporting Failed"),
                    /*: An error message. %1 is a stand-in argument for the name
                        of the file we attempted to save to. */
                    self->tr("There was an error trying to save the address list to %1. Please try again.").arg(filename),
                    QMessageBox::Ok, self);
                box->setObjectName(QStringLiteral("addressBookExportFailed"));
                GUIUtil::ShowModalDialogAsynchronously(box);
            }
        });
}

void AddressBookPage::contextualMenu(const QPoint &point)
{
    QModelIndex index = ui->tableView->indexAt(point);
    if(index.isValid())
    {
        contextMenu->exec(QCursor::pos());
    }
}

void AddressBookPage::selectNewAddress(const QModelIndex &parent, int begin, int /*end*/)
{
    QModelIndex idx = proxyModel->mapFromSource(model->index(begin, AddressTableModel::Address, parent));
    if(idx.isValid() && (idx.data(Qt::EditRole).toString() == newAddressToSelect))
    {
        // Select row of newly created address, once
        ui->tableView->setFocus();
        ui->tableView->selectRow(idx.row());
        newAddressToSelect.clear();
    }
}

void AddressBookPage::updateWindowsTitleWithVaultName()
{
    const QString vaultName = this->model->GetVaultDisplayName();

    if (mode == ForEditing) {
        switch(tab)
        {
        case SendingTab: setWindowTitle(tr("Destination addresses - %1").arg(vaultName)); break;
        case ReceivingTab: setWindowTitle(tr("Receiving addresses - %1").arg(vaultName)); break;
        }
    }
}
