// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef QUICKSILVER_QT_GPUSITUATION_H
#define QUICKSILVER_QT_GPUSITUATION_H

#include <crypto/cuckatoo/gpu_devices.h>

#include <QMetaType>
#include <QMap>
#include <QString>

#include <cstdint>
#include <functional>
#include <optional>

class QObject;

Q_DECLARE_METATYPE(cuckatoo::GpuDiscovery)

namespace gpusituation {
void RunDiscovery(uint8_t edgebits, QObject* receiver, std::function<void(const cuckatoo::GpuDiscovery&)> done);
QString ReasonClause(const cuckatoo::GpuDevice& device);
enum class RowMode { kAutomatic, kChoose };
QString RowState(const cuckatoo::GpuDevice& device, RowMode mode, bool ticked);
QString MissingRowState();
struct StartupNotice { QString headline; QString informative; bool offer_settings; };
std::optional<StartupNotice> StartupNoticeFor(const cuckatoo::GpuDiscovery& discovery, bool windows,
                                              const QMap<QString, QString>& names);
struct SendNote { QString text; bool processor_fallback; };
SendNote SendNoteFor(const std::optional<cuckatoo::GpuDiscovery>& discovery, bool sandbox);
bool CardWillWork(const std::optional<cuckatoo::GpuDiscovery>& discovery);
QString SlotOf(const std::string& id);
} // namespace gpusituation

#endif // QUICKSILVER_QT_GPUSITUATION_H
