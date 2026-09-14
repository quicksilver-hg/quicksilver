// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/maturity.h>

#include <QCoreApplication>
#include <QObject>

namespace qsmaturity {

QString FormatMaturityCountdown(int blocks_remaining, int64_t target_spacing_seconds)
{
    if (blocks_remaining <= 0 || target_spacing_seconds <= 0) return QString();

    const int64_t seconds = static_cast<int64_t>(blocks_remaining) * target_spacing_seconds;

    // Singular and plural are separate strings rather than the "%n unit(s)" idiom
    // used elsewhere in this directory. That idiom only reads correctly once a
    // translator supplies the numerus forms: with none installed -- which is the
    // case in the Qt test binary, and for any user whose locale has no catalogue
    // -- Qt substitutes %n and leaves the literal "(s)", so the headline first-run
    // string would render "spendable in about 8 hour(s)". Both forms below are
    // still translatable; neither depends on a catalogue to be grammatical.
    if (seconds < 3600) {
        const int minutes = static_cast<int>((seconds + 30) / 60);
        return minutes == 1 ? QObject::tr("spendable in about 1 minute")
                            : QObject::tr("spendable in about %1 minutes").arg(minutes);
    }
    // Round to the nearest hour rather than truncating: an honest "about 3 hours"
    // beats a technically-true "2 hours" that expires an hour late.
    const int hours = static_cast<int>((seconds + 1800) / 3600);
    return hours == 1 ? QObject::tr("spendable in about 1 hour")
                      : QObject::tr("spendable in about %1 hours").arg(hours);
}

} // namespace qsmaturity
