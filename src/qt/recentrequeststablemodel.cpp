// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/recentrequeststablemodel.h>

#include <qt/quicksilverunits.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/vaultmodel.h>

#include <clientversion.h>
#include <interfaces/vault.h>
#include <key_io.h>
#include <streams.h>
#include <logging.h>
#include <util/string.h>

#include <exception>

#include <utility>

#include <QLatin1Char>
#include <QLatin1String>

using util::ToString;

RecentRequestsTableModel::RecentRequestsTableModel(VaultModel *parent) :
    QAbstractTableModel(parent), vaultModel(parent)
{
    // Load entries from vault. A record that does not read is skipped and
    // left in the vault, so one corrupt `rr*` value cannot end the app (S0240-C-01).
    int skipped{0};
    for (const std::string& request : parent->vault().getAddressReceiveRequests()) {
        if (!addNewRequest(request)) ++skipped;
    }
    if (skipped > 0) {
        LogWarning(HgLog::QT, "Receive requests: skipped %d stored request(s) that could not be read; they are kept in the vault\n", skipped);
    }

    /* These columns must match the indices in the ColumnIndex enumeration */
    columns << tr("Date") << tr("Label") << tr("Message") << getAmountTitle();

    connect(vaultModel->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &RecentRequestsTableModel::updateDisplayUnit);
}

RecentRequestsTableModel::~RecentRequestsTableModel() = default;

int RecentRequestsTableModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return list.length();
}

int RecentRequestsTableModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return columns.length();
}

QVariant RecentRequestsTableModel::data(const QModelIndex &index, int role) const
{
    if(!index.isValid() || index.row() >= list.length())
        return QVariant();

    if(role == Qt::DisplayRole || role == Qt::EditRole)
    {
        const RecentRequestEntry *rec = &list[index.row()];
        switch(index.column())
        {
        case Date:
            return GUIUtil::dateTimeStr(rec->date);
        case Label:
            if(rec->recipient.label.isEmpty() && role == Qt::DisplayRole)
            {
                return tr("(no label)");
            }
            else
            {
                return rec->recipient.label;
            }
        case Message:
            if(rec->recipient.message.isEmpty() && role == Qt::DisplayRole)
            {
                return tr("(no message)");
            }
            else
            {
                return rec->recipient.message;
            }
        case Amount:
            if (rec->recipient.amount == 0 && role == Qt::DisplayRole)
                return tr("(no amount requested)");
            else if (role == Qt::EditRole)
                return QuicksilverUnits::format(vaultModel->getOptionsModel()->getDisplayUnit(), rec->recipient.amount, false, QuicksilverUnits::SeparatorStyle::NEVER);
            else
                return QuicksilverUnits::format(vaultModel->getOptionsModel()->getDisplayUnit(), rec->recipient.amount);
        }
    }
    else if (role == Qt::TextAlignmentRole)
    {
        if (index.column() == Amount)
            return (int)(Qt::AlignRight|Qt::AlignVCenter);
    }
    return QVariant();
}

bool RecentRequestsTableModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    return true;
}

QVariant RecentRequestsTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if(orientation == Qt::Horizontal)
    {
        if(role == Qt::DisplayRole && section < columns.size())
        {
            return columns[section];
        }
    }
    return QVariant();
}

/** Updates the column title to "Amount (DisplayUnit)" and emits headerDataChanged() signal for table headers to react. */
void RecentRequestsTableModel::updateAmountColumnTitle()
{
    columns[Amount] = getAmountTitle();
    Q_EMIT headerDataChanged(Qt::Horizontal,Amount,Amount);
}

/** Gets title for amount column including current display unit if optionsModel reference available. */
QString RecentRequestsTableModel::getAmountTitle()
{
    if (!vaultModel->getOptionsModel()) return {};
    return tr("Requested") +
           QLatin1String(" (") +
           QuicksilverUnits::shortName(this->vaultModel->getOptionsModel()->getDisplayUnit()) +
           QLatin1Char(')');
}

QModelIndex RecentRequestsTableModel::index(int row, int column, const QModelIndex &parent) const
{
    Q_UNUSED(parent);

    return createIndex(row, column);
}

bool RecentRequestsTableModel::removeRows(int row, int count, const QModelIndex &parent)
{
    if (parent.isValid() || count <= 0 || row < 0 || (row + count) > list.size()) {
        return false;
    }
    // Row by row, so a failed delete leaves listed exactly the requests the
    // vault still holds (S0240-C-02).
    for (int i = 0; i < count; ++i) {
        const RecentRequestEntry& rec = list[row];
        if (!vaultModel->vault().setAddressReceiveRequest(DecodeDestination(rec.recipient.address.toStdString()), ToString(rec.id), "")) {
            return false;
        }
        beginRemoveRows(parent, row, row);
        list.removeAt(row);
        endRemoveRows();
    }
    return true;
}

Qt::ItemFlags RecentRequestsTableModel::flags(const QModelIndex &index) const
{
    return Qt::ItemIsSelectable | Qt::ItemIsEnabled;
}

// called when adding a request from the GUI
bool RecentRequestsTableModel::addNewRequest(const SendCoinsRecipient &recipient)
{
    RecentRequestEntry newEntry;
    newEntry.id = ++nReceiveRequestsMaxId;
    newEntry.date = QDateTime::currentDateTime();
    newEntry.recipient = recipient;

    DataStream ss{};
    ss << newEntry;

    if (!vaultModel->vault().setAddressReceiveRequest(DecodeDestination(recipient.address.toStdString()), ToString(newEntry.id), ss.str()))
        return false;

    addNewRequest(newEntry);
    return true;
}

// called from ctor when loading from vault
bool RecentRequestsTableModel::addNewRequest(const std::string &recipient)
{
    std::vector<uint8_t> data(recipient.begin(), recipient.end());
    DataStream ss{data};

    RecentRequestEntry entry;
    try {
        ss >> entry;
    } catch (const std::exception&) {
        return false;
    }

    if (entry.id <= 0) // should not happen
        return false;

    if (entry.id > nReceiveRequestsMaxId)
        nReceiveRequestsMaxId = entry.id;

    addNewRequest(entry);
    return true;
}

// actually add to table in GUI
void RecentRequestsTableModel::addNewRequest(RecentRequestEntry &recipient)
{
    beginInsertRows(QModelIndex(), 0, 0);
    list.prepend(recipient);
    endInsertRows();
}

void RecentRequestsTableModel::sort(int column, Qt::SortOrder order)
{
    // An empty list has no row to report changed (S0240-C-06).
    if (list.isEmpty()) return;
    std::sort(list.begin(), list.end(), RecentRequestEntryLessThan(column, order));
    Q_EMIT dataChanged(index(0, 0, QModelIndex()), index(list.size() - 1, NUMBER_OF_COLUMNS - 1, QModelIndex()));
}

void RecentRequestsTableModel::updateDisplayUnit()
{
    updateAmountColumnTitle();
}

bool RecentRequestEntryLessThan::operator()(const RecentRequestEntry& left, const RecentRequestEntry& right) const
{
    const RecentRequestEntry* pLeft = &left;
    const RecentRequestEntry* pRight = &right;
    if (order == Qt::DescendingOrder)
        std::swap(pLeft, pRight);

    switch(column)
    {
    case RecentRequestsTableModel::Date:
        return pLeft->date.toSecsSinceEpoch() < pRight->date.toSecsSinceEpoch();
    case RecentRequestsTableModel::Label:
        return pLeft->recipient.label < pRight->recipient.label;
    case RecentRequestsTableModel::Message:
        return pLeft->recipient.message < pRight->recipient.message;
    case RecentRequestsTableModel::Amount:
        return pLeft->recipient.amount < pRight->recipient.amount;
    default:
        return pLeft->id < pRight->id;
    }
}
