// Copyright (c) 2013-2020 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_NOUI_H
#define QUICKSILVER_NOUI_H

#include <string>

struct bilingual_str;

/** Non-GUI handler, which logs and prints messages. */
bool noui_ThreadSafeMessageBox(const bilingual_str& message, const std::string& caption, unsigned int style);
/** Non-GUI handler, which logs and prints questions. */
bool noui_ThreadSafeQuestion(const bilingual_str& /* ignored interactive message */, const std::string& message, const std::string& caption, unsigned int style);
/** Non-GUI handler, which only logs a message. */
void noui_InitMessage(const std::string& message);

/** Connect all quicksilver-daemon signal handlers */
void noui_connect();

/** Redirect all quicksilver-daemon signal handlers to the log. Used to check or suppress output during test runs that produce expected errors */
void noui_test_redirect();

/** Reconnects the regular Non-GUI handlers after having used noui_test_redirect */
void noui_reconnect();

#endif // QUICKSILVER_NOUI_H
