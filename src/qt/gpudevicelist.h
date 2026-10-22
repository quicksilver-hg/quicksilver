// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef QUICKSILVER_QT_GPUDEVICELIST_H
#define QUICKSILVER_QT_GPUDEVICELIST_H

#include <crypto/cuckatoo/gpu_devices.h>

#include <QMap>
#include <QStringList>
#include <QWidget>

class QCheckBox;
class QLabel;
class QVBoxLayout;

class GpuDeviceList : public QWidget
{
    Q_OBJECT
public:
    enum class Mode { kAutomatic, kChoose, kCpuOnly };
    explicit GpuDeviceList(QWidget* parent = nullptr);
    void setDevices(const cuckatoo::GpuDiscovery& discovery, const QStringList& chosen, const QMap<QString, QString>& names);
    void setListingState(const QString& text);
    void setMode(Mode mode);
    QStringList tickedIds() const;
    QMap<QString, QString> tickedNames() const;
    void setChecked(const QString& id, bool checked);
    void refreshRows();
Q_SIGNALS:
    void changed();
    void integratedChecked(const QString& id);
private:
    struct Row { QString id; QString name; bool integrated; bool missing; QCheckBox* check; QLabel* state; cuckatoo::GpuDevice device; };
    void clearRows();
    QVBoxLayout* m_layout;
    QLabel* m_state;
    QList<Row> m_rows;
    Mode m_mode{Mode::kAutomatic};
};

#endif // QUICKSILVER_QT_GPUDEVICELIST_H
