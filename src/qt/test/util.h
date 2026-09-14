// Copyright (c) 2018-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_TEST_UTIL_H
#define QUICKSILVER_QT_TEST_UTIL_H

#include <functional>

/**
 * Run `show_dialog` and require that it returns while a modal widget of
 * `class_name` is still the active modal. Fails if `show_dialog` used
 * QDialog::exec() (nested event loop). Closes the modal afterwards.
 */
void ExpectModalWithoutNestedEventLoop(const char* class_name, const std::function<void()>& show_dialog);

#endif // QUICKSILVER_QT_TEST_UTIL_H
