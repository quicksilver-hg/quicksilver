// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_UTIL_UTF16_H
#define QUICKSILVER_UTIL_UTF16_H

#ifdef WIN32

#include <string>
#include <string_view>

namespace util {

std::wstring Utf8ToUtf16(std::string_view in);
std::string Utf16ToUtf8(std::wstring_view in);

} // namespace util

#endif // WIN32

#endif // QUICKSILVER_UTIL_UTF16_H
