// Copyright (c) 2018-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/quicksilver.h>

#include <compat/compat.h>
#include <util/translation.h>

#include <QCoreApplication>

#include <string>

/** Translate string to current locale using Qt. */
extern const TranslateFn G_TRANSLATION_FUN = [](const char* psz) {
    return QCoreApplication::translate("quicksilver", psz).toStdString();
};

MAIN_FUNCTION
{
    return GuiMain(argc, argv);
}
