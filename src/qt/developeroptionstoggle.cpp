// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/developeroptionstoggle.h>

#include <QVariant>

DeveloperOptionsToggle::DeveloperOptionsToggle(QWidget* parent)
    : QPushButton(parent)
{
    setProperty("class", QStringLiteral("benchQuiet"));
    setCheckable(true);
    setChecked(false);
    connect(this, &QAbstractButton::toggled, this, &DeveloperOptionsToggle::updateText);
    updateText();
}

void DeveloperOptionsToggle::setForcedOpen(const QString& reason)
{
    if (!reason.isEmpty()) setChecked(true);
    setEnabled(reason.isEmpty());
    setToolTip(reason);
}

void DeveloperOptionsToggle::updateText()
{
    setText(isChecked() ? tr("− Hide developer options") : tr("+ Show developer options"));
}
