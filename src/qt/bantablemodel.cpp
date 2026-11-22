// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/bantablemodel.h>

#include <interfaces/node.h>
#include <net_types.h> // For banmap_t

#include <utility>

#include <QDateTime>
#include <QList>
#include <QLocale>
#include <QModelIndex>
#include <QVariant>

bool BannedNodeLessThan::operator()(const CCombinedBan& left, const CCombinedBan& right) const
{
    const CCombinedBan* pLeft = &left;
    const CCombinedBan* pRight = &right;

    if (order == Qt::DescendingOrder)
        std::swap(pLeft, pRight);

    switch (static_cast<BanTableModel::ColumnIndex>(column)) {
    case BanTableModel::Address:
        return pLeft->subnet.ToString().compare(pRight->subnet.ToString()) < 0;
    case BanTableModel::Bantime:
        return pLeft->banEntry.nBanUntil < pRight->banEntry.nBanUntil;
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

// private implementation
class BanTablePriv
{
public:
    /** Local cache of peer information */
    QList<CCombinedBan> cachedBanlist;
    /** Column to sort nodes by (default to unsorted) */
    int sortColumn{-1};
    /** Order (ascending or descending) to sort nodes by */
    Qt::SortOrder sortOrder;

    /** Pull a full list of banned nodes from interfaces::Node into our cache */
    void refreshBanlist(interfaces::Node& node)
    {
        banmap_t banMap;
        node.getBanned(banMap);

        cachedBanlist.clear();
        cachedBanlist.reserve(banMap.size());
        for (const auto& entry : banMap)
        {
            CCombinedBan banEntry;
            banEntry.subnet = entry.first;
            banEntry.banEntry = entry.second;
            cachedBanlist.append(banEntry);
        }

        if (sortColumn >= 0)
            // sort cachedBanlist (use stable sort to prevent rows jumping around unnecessarily)
            std::stable_sort(cachedBanlist.begin(), cachedBanlist.end(), BannedNodeLessThan(sortColumn, sortOrder));
    }

    int size() const
    {
        return cachedBanlist.size();
    }

    CCombinedBan *index(int idx)
    {
        if (idx >= 0 && idx < cachedBanlist.size())
            return &cachedBanlist[idx];

        return nullptr;
    }
};

BanTableModel::BanTableModel(interfaces::Node& node, QObject* parent) :
    QAbstractTableModel(parent),
    m_node(node)
{
    columns << tr("IP/Netmask") << tr("Banned Until");
    priv.reset(new BanTablePriv());

    // load initial data
    refresh();
}

BanTableModel::~BanTableModel() = default;

int BanTableModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return priv->size();
}

int BanTableModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return columns.length();
}

QVariant BanTableModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.model() != this || index.row() < 0 || index.row() >= priv->size() ||
        index.column() < 0 || index.column() >= columnCount({}))
        return QVariant();

    CCombinedBan* rec = priv->index(index.row());

    const auto column = static_cast<ColumnIndex>(index.column());
    if (role == Qt::DisplayRole) {
        switch (column) {
        case Address:
            return QString::fromStdString(rec->subnet.ToString());
        case Bantime:
            QDateTime date = QDateTime::fromMSecsSinceEpoch(0);
            date = date.addSecs(rec->banEntry.nBanUntil);
            return QLocale::system().toString(date, QLocale::LongFormat);
        } // no default case, so the compiler can warn about missing cases
        assert(false);
    }

    return QVariant();
}

QVariant BanTableModel::headerData(int section, Qt::Orientation orientation, int role) const
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

Qt::ItemFlags BanTableModel::flags(const QModelIndex &index) const
{
    if (!index.isValid()) return Qt::NoItemFlags;

    Qt::ItemFlags retval = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
    return retval;
}

QModelIndex BanTableModel::index(int row, int column, const QModelIndex &parent) const
{
    if (!parent.isValid() && row >= 0 && row < rowCount({}) && column >= 0 && column < columnCount({}))
        return createIndex(row, column);
    return QModelIndex();
}

void BanTableModel::refresh()
{
    Q_EMIT layoutAboutToBeChanged();
    const auto old_indexes = persistentIndexList();
    QList<CSubNet> old_subnets;
    old_subnets.reserve(old_indexes.size());
    for (const auto& index : old_indexes) {
        old_subnets.append(priv->cachedBanlist.at(index.row()).subnet);
    }
    priv->refreshBanlist(m_node);
    QModelIndexList new_indexes;
    new_indexes.reserve(old_indexes.size());
    for (int i = 0; i < old_indexes.size(); ++i) {
        QModelIndex mapped;
        for (int row = 0; row < priv->size(); ++row) {
            if (priv->cachedBanlist.at(row).subnet == old_subnets.at(i)) {
                mapped = index(row, old_indexes.at(i).column(), {});
                break;
            }
        }
        new_indexes.append(mapped);
    }
    changePersistentIndexList(old_indexes, new_indexes);
    Q_EMIT layoutChanged();
}

void BanTableModel::sort(int column, Qt::SortOrder order)
{
    priv->sortColumn = column;
    priv->sortOrder = order;
    refresh();
}

bool BanTableModel::shouldShow()
{
    return priv->size() > 0;
}

bool BanTableModel::unban(const QModelIndex& index)
{
    if (!index.isValid() || index.model() != this || index.row() < 0 || index.row() >= priv->size()) return false;
    return m_node.unban(priv->cachedBanlist.at(index.row()).subnet);
}
