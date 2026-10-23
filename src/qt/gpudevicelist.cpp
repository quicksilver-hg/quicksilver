// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/gpudevicelist.h>
#include <qt/gpusituation.h>
#include <crypto/cuckatoo/multi_device.h>
#include <crypto/cuckatoo/gpu_solver.h>

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace {
QString ObjectSuffix(QString id)
{
    return id.replace(QLatin1Char(':'), QLatin1Char('_')).replace(QLatin1Char('.'), QLatin1Char('_'));
}
}

GpuDeviceList::GpuDeviceList(QWidget* parent) : QWidget(parent), m_layout(new QVBoxLayout(this)), m_state(new QLabel(this))
{
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->addWidget(m_state);
    auto* refresh = new QTimer(this);
    refresh->setInterval(1000);
    connect(refresh, &QTimer::timeout, this, &GpuDeviceList::refreshRows);
    refresh->start();
}

void GpuDeviceList::clearRows()
{
    m_rows.clear();
    while (QLayoutItem* item = m_layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) widget->deleteLater();
        delete item;
    }
    m_state = new QLabel(this);
    m_state->setWordWrap(true);
    m_layout->addWidget(m_state);
}

void GpuDeviceList::setListingState(const QString& text)
{
    clearRows();
    m_state->setText(text);
}

void GpuDeviceList::setDevices(const cuckatoo::GpuDiscovery& discovery, const QStringList& chosen, const QMap<QString, QString>& names)
{
    clearRows();
    m_state->hide();
    std::vector<cuckatoo::GpuDevice> devices;
    for (const auto& device : discovery.listing.devices) if (device.type == "gpu") devices.push_back(device);
    std::sort(devices.begin(), devices.end(), [](const auto& a, const auto& b) { return cuckatoo::DeviceIdLess(a.id, b.id); });

    QMap<QString, int> name_counts;
    for (const auto& device : devices) ++name_counts[QString::fromStdString(device.name)];
    for (const QString& id : chosen) {
        if (id == QStringLiteral("cpu")) continue;
        const bool found = std::any_of(devices.begin(), devices.end(), [&id](const auto& device) { return QString::fromStdString(device.id) == id; });
        if (!found) ++name_counts[names.value(id, id)];
    }
    QStringList seen;
    const auto add = [this, &name_counts](const QString& id, const QString& name, bool integrated, bool missing, const cuckatoo::GpuDevice& device, bool ticked) {
        auto* line = new QWidget(this);
        auto* layout = new QHBoxLayout(line);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* check = new QCheckBox(name_counts.value(name) > 1 ? QStringLiteral("%1 (%2)").arg(name, id) : name, line);
        const QString object{QStringLiteral("gpuDevice_") + ObjectSuffix(id)};
        check->setObjectName(object);
        check->setChecked(ticked);
        auto* state = new QLabel(line);
        state->setObjectName(object + QStringLiteral("_state"));
        layout->addWidget(check, 1);
        layout->addWidget(state);
        const QString detail{missing ? id : QStringLiteral("%1\n%2").arg(id, QString::fromStdString(device.reason))};
        line->setToolTip(detail.trimmed());
        m_layout->addWidget(line);
        m_rows.push_back({id, name, integrated, missing, check, state, device});
        connect(check, &QCheckBox::toggled, this, [this, id, integrated](bool on) {
            refreshRows();
            if (on && integrated && m_mode == Mode::kChoose) Q_EMIT integratedChecked(id);
            Q_EMIT changed();
        });
    };
    for (const auto& device : devices) {
        const QString id{QString::fromStdString(device.id)};
        seen.push_back(id);
        const bool ticked{chosen.contains(id) || (chosen.isEmpty() && device.status == cuckatoo::DeviceStatus::kOk && !device.integrated)};
        add(id, QString::fromStdString(device.name), device.integrated, false, device, ticked);
    }
    for (const QString& id : chosen) {
        if (id == QStringLiteral("cpu") || seen.contains(id)) continue;
        cuckatoo::GpuDevice missing;
        missing.id = id.toStdString();
        missing.name = names.value(id, id).toStdString();
        add(id, names.value(id, id), false, true, missing, true);
    }
    refreshRows();
}

void GpuDeviceList::setMode(Mode mode)
{
    m_mode = mode;
    setVisible(mode != Mode::kCpuOnly);
    refreshRows();
}

void GpuDeviceList::setMiningActive(bool active)
{
    m_mining_active = active;
    refreshRows();
}

void GpuDeviceList::refreshRows()
{
    const auto stats = cuckatoo::GetGpuDeviceStats();
    for (Row& row : m_rows) {
        row.check->setEnabled(m_mode == Mode::kChoose && !row.missing && row.device.status == cuckatoo::DeviceStatus::kOk);
        QString state{row.missing ? gpusituation::MissingRowState() : gpusituation::RowState(row.device, m_mode == Mode::kAutomatic ? gpusituation::RowMode::kAutomatic : gpusituation::RowMode::kChoose, row.check->isChecked())};
        const bool chosen = !row.missing && row.device.status == cuckatoo::DeviceStatus::kOk &&
            (m_mode == Mode::kAutomatic ? !row.integrated : m_mode == Mode::kChoose && row.check->isChecked());
        if (chosen && !m_mining_active) state = tr("Chosen");
        if (chosen && m_mining_active) {
            const auto it = std::find_if(stats.begin(), stats.end(), [&row](const auto& item) { return item.device == row.device.id; });
            if (it != stats.end()) {
                if (it->state == cuckatoo::GpuDeviceState::kFaulted) {
                    state = tr("Faulted: %1").arg(QString::fromStdString(cuckatoo::SolverFault(it->last_status, false).value_or("")));
                } else if (it->mean_seconds_per_graph > 0) {
                    state = tr("In use, %1 s per graph").arg(it->mean_seconds_per_graph, 0, 'f', 1);
                }
            }
        }
        row.state->setText(state);
    }
}

QStringList GpuDeviceList::tickedIds() const
{
    QStringList out;
    for (const Row& row : m_rows) if (row.check->isChecked() && !row.missing) out.push_back(row.id);
    return out;
}

QMap<QString, QString> GpuDeviceList::tickedNames() const
{
    QMap<QString, QString> out;
    for (const Row& row : m_rows) if (row.check->isChecked() && !row.missing) out.insert(row.id, row.name);
    return out;
}

void GpuDeviceList::setChecked(const QString& id, bool checked)
{
    for (Row& row : m_rows) if (row.id == id) row.check->setChecked(checked);
}
