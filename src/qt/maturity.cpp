// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/maturity.h>

#include <QCoreApplication>
#include <QObject>

#include <algorithm>

namespace qsmaturity {

namespace {

/** A wait rounded the way every maturity string phrases it. */
struct RoundedWait {
    int value;
    bool hours;
};

RoundedWait RoundWait(int blocks_remaining, int64_t target_spacing_seconds)
{
    const int64_t seconds = static_cast<int64_t>(blocks_remaining) * target_spacing_seconds;
    // Under an hour, the nearest minute. Otherwise round to the nearest hour rather than
    // truncating: an honest "about 3 hours" beats a technically-true "2 hours" that
    // expires an hour late.
    if (seconds < 3600) return {static_cast<int>((seconds + 30) / 60), false};
    return {static_cast<int>((seconds + 1800) / 3600), true};
}

// Singular and plural are separate strings rather than the "%n unit(s)" idiom
// used elsewhere in this directory. That idiom only reads correctly once a
// translator supplies the numerus forms: with none installed -- which is the
// case in the Qt test binary, and for any user whose locale has no catalogue
// -- Qt substitutes %n and leaves the literal "(s)", so the headline first-run
// string would render "spendable in about 8 hour(s)". Every form below is
// still translatable; none depends on a catalogue to be grammatical.
QString WaitPhrase(const RoundedWait& wait)
{
    if (wait.hours) {
        return wait.value == 1 ? QObject::tr("1 hour") : QObject::tr("%1 hours").arg(wait.value);
    }
    return wait.value == 1 ? QObject::tr("1 minute") : QObject::tr("%1 minutes").arg(wait.value);
}

} // namespace

QString FormatMaturityCountdown(int blocks_remaining, int64_t target_spacing_seconds)
{
    if (blocks_remaining <= 0 || target_spacing_seconds <= 0) return QString();

    // Whole sentences, so a translator can place the number freely.
    const RoundedWait wait{RoundWait(blocks_remaining, target_spacing_seconds)};
    if (!wait.hours) {
        return wait.value == 1 ? QObject::tr("spendable in about 1 minute")
                               : QObject::tr("spendable in about %1 minutes").arg(wait.value);
    }
    return wait.value == 1 ? QObject::tr("spendable in about 1 hour")
                           : QObject::tr("spendable in about %1 hours").arg(wait.value);
}

MaturingSummary SummarizeMaturing(const std::vector<std::pair<int, CAmount>>& rows)
{
    MaturingSummary summary;
    for (const auto& [blocks_remaining, amount] : rows) {
        if (blocks_remaining <= 0) continue;
        if (summary.soonest == 0 || blocks_remaining < summary.soonest) {
            summary.soonest = blocks_remaining;
            summary.soonest_amount = amount;
        } else if (blocks_remaining == summary.soonest) {
            summary.soonest_amount += amount;
        }
        summary.latest = std::max(summary.latest, blocks_remaining);
    }
    return summary;
}

QString FormatMaturingHint(int soonest, const QString& soonest_amount, int latest, int64_t target_spacing_seconds)
{
    if (soonest <= 0 || target_spacing_seconds <= 0) return QString();
    if (latest <= soonest) return FormatMaturityCountdown(soonest, target_spacing_seconds);
    return QObject::tr("next %1 in about %2 \u00b7 all in about %3")
        .arg(soonest_amount,
             WaitPhrase(RoundWait(soonest, target_spacing_seconds)),
             WaitPhrase(RoundWait(latest, target_spacing_seconds)));
}

QString FormatMaturingHint(const MaturingSummary& summary, QuicksilverUnit unit,
                           QuicksilverUnits::SeparatorStyle separators, bool privacy,
                           int64_t target_spacing_seconds)
{
    const QString amount = QuicksilverUnits::formatInlineWithPrivacy(unit, summary.soonest_amount, separators, privacy);
    return FormatMaturingHint(summary.soonest, amount, summary.latest, target_spacing_seconds);
}

} // namespace qsmaturity
