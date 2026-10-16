// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/gpusituation.h>

#include <QCoreApplication>
#include <QMetaObject>
#include <QObject>
#include <QPointer>

#include <thread>

namespace gpusituation {

void RunDiscovery(uint8_t edgebits, QObject* receiver, std::function<void(const cuckatoo::GpuDiscovery&)> done)
{
    const QPointer<QObject> guarded{receiver};
    std::thread([edgebits, guarded, done = std::move(done)] {
        const cuckatoo::GpuDiscovery discovery{cuckatoo::DiscoverGpus(edgebits)};
        QCoreApplication* app{QCoreApplication::instance()};
        if (!app) return;
        QMetaObject::invokeMethod(app, [guarded, done, discovery] {
            if (guarded) done(discovery);
        }, Qt::QueuedConnection);
    }).detach();
}

QString ReasonClause(const cuckatoo::GpuDevice& device)
{
    if (device.status == cuckatoo::DeviceStatus::kNoDriver) {
        return QObject::tr("its OpenCL driver isn't installed.");
    }
    return QObject::tr("its driver couldn't run Quicksilver's graphics code.");
}

QString RowState(const cuckatoo::GpuDevice& device, RowMode mode, bool ticked)
{
    if (device.status != cuckatoo::DeviceStatus::kOk) {
        QString reason{ReasonClause(device)};
        if (reason.endsWith(QLatin1Char('.'))) reason.chop(1);
        return QObject::tr("Can't be used: %1").arg(reason);
    }
    if (mode == RowMode::kAutomatic) {
        return device.integrated ? QObject::tr("Not used: built-in graphics") : QObject::tr("In use");
    }
    return ticked ? QObject::tr("In use") : QObject::tr("Not chosen");
}

QString MissingRowState() { return QObject::tr("Not found"); }

QString SlotOf(const std::string& id)
{
    if (!id.starts_with("pci:") || id.size() != 16) return {};
    return QString::fromStdString(id.substr(9));
}

std::optional<StartupNotice> StartupNoticeFor(const cuckatoo::GpuDiscovery& discovery, bool windows,
                                              const QMap<QString, QString>& names)
{
    StartupNotice out;
    switch (discovery.situation) {
    case cuckatoo::GpuSituation::kNoGpu:
        out = {QObject::tr("No graphics card was found on this computer."),
               QObject::tr("Quicksilver will prepare transfers on this computer's processor. You can stop the work at any time."), false};
        break;
    case cuckatoo::GpuSituation::kIntegratedOnly:
        out = {QObject::tr("This computer has only built-in graphics."),
               QObject::tr("By default Quicksilver will use the processor. Built-in graphics may be selected in Settings."), true};
        break;
    case cuckatoo::GpuSituation::kNoneUsable: {
        const bool no_driver{!discovery.selection.unusable.empty() && discovery.selection.unusable.front().status == cuckatoo::DeviceStatus::kNoDriver};
        out.headline = QObject::tr("Quicksilver is unable to use this computer's graphics card.");
        out.informative = no_driver
            ? (windows ? QObject::tr("Install the latest driver from your card's maker.")
                       : QObject::tr("Install your card's OpenCL driver. On Ubuntu, AMD cards use the <i>mesa-opencl-icd</i> package, Intel cards use <i>intel-opencl-icd</i>, and NVIDIA cards use NVIDIA's driver."))
            : QObject::tr("This computer's graphics driver is not compatible with Quicksilver. Updating the driver may fix this.");
        out.offer_settings = true;
        break;
    }
    case cuckatoo::GpuSituation::kChosenMissing: {
        const std::string id{discovery.selection.missing.empty() ? std::string{} : discovery.selection.missing.front()};
        const QString qid{QString::fromStdString(id)};
        const QString name{names.value(qid, qid)};
        const QString slot{SlotOf(id)};
        out.headline = slot.isEmpty()
            ? QObject::tr("The graphics card chosen in Settings, %1, wasn't found.").arg(name)
            : QObject::tr("The graphics card chosen in Settings, %1 (slot %2), wasn't found.").arg(name, slot);
        out.informative = QObject::tr("It may have been removed or moved to another slot. Select a graphics card in Settings.");
        out.offer_settings = true;
        break;
    }
    case cuckatoo::GpuSituation::kNoLoader:
        out = {QObject::tr("Quicksilver couldn't check this computer's graphics cards, because no OpenCL driver is installed."),
               windows ? QObject::tr("If this computer has a graphics card and you'd like Quicksilver to use it, install the latest driver from the card's maker.")
                       : QObject::tr("If this computer has a graphics card and you'd like Quicksilver to use it, install the card's OpenCL driver: <i>mesa-opencl-icd</i> for AMD, <i>intel-opencl-icd</i> for Intel, or NVIDIA's own driver for NVIDIA."), false};
        break;
    case cuckatoo::GpuSituation::kHelperMissing:
        out = {QObject::tr("Quicksilver's graphics helper isn't installed."),
               windows ? QObject::tr("Reinstall Quicksilver.") : QObject::tr("Install it with <i>sudo apt install quicksilver-gpu</i>."), false};
        break;
    default:
        return std::nullopt;
    }
    return out;
}

SendNote SendNoteFor(const std::optional<cuckatoo::GpuDiscovery>& discovery, bool sandbox)
{
    if (sandbox) return {QObject::tr("Sandbox transfers use quick built-in preparation and do not need graphics acceleration."), false};
    if (!discovery) return {QObject::tr("Checking this computer's graphics cards…"), false};
    const QString timing{QObject::tr("Preparing a transfer usually takes one to two minutes, but the search is random: some transfers finish in seconds and some run past five minutes. You can stop the work at any time.")};
    const QString display{QObject::tr("Your screen may respond slowly while transfers are being prepared.")};
    switch (discovery->situation) {
    case cuckatoo::GpuSituation::kReady: {
        const int count{static_cast<int>(discovery->selection.use.size())};
        const QString ready{count == 1 ? QObject::tr("Graphics acceleration is ready (1 card).")
                                       : QObject::tr("Graphics acceleration is ready (%1 cards).").arg(count)};
        return {ready + QLatin1Char(' ') + timing + QLatin1Char(' ') + display, false};
    }
    case cuckatoo::GpuSituation::kPartial: {
        const int used{static_cast<int>(discovery->selection.use.size())};
        const int total{used + static_cast<int>(discovery->selection.unusable.size() + discovery->selection.missing.size())};
        QString name;
        QString reason;
        if (!discovery->selection.unusable.empty()) {
            name = QString::fromStdString(discovery->selection.unusable.front().name);
            reason = ReasonClause(discovery->selection.unusable.front());
        } else {
            name = QString::fromStdString(discovery->selection.missing.front());
            reason = QObject::tr("it wasn't found.");
        }
        return {QObject::tr("Using %1 of %2 graphics cards. %3 can't be used: %4 See Settings.").arg(used).arg(total).arg(name, reason)
                    + QLatin1Char(' ') + timing + QLatin1Char(' ') + display, false};
    }
    case cuckatoo::GpuSituation::kCpuChosen:
        return {QObject::tr("Graphics cards are turned off in Settings, so this computer's processor will prepare transfers. This may take many minutes."), true};
    case cuckatoo::GpuSituation::kListingFailed:
        return {QObject::tr("Quicksilver was unable to detect a graphics card on this computer, so this computer's processor will prepare transfers. This may take many minutes."), true};
    default:
        return {QObject::tr("Graphics acceleration isn't available, so this computer's processor will prepare transfers. This may take many minutes."), true};
    }
}

bool CardWillWork(const std::optional<cuckatoo::GpuDiscovery>& discovery)
{
    return discovery && (discovery->situation == cuckatoo::GpuSituation::kReady || discovery->situation == cuckatoo::GpuSituation::kPartial);
}

} // namespace gpusituation
