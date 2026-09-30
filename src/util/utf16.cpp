// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/utf16.h>

#ifdef WIN32

#include <compat/compat.h>

#include <charconv>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>

namespace util {
namespace {

[[noreturn]] void ThrowConversionError(const char* operation, DWORD error)
{
    char error_text[std::numeric_limits<DWORD>::digits10 + 2];
    const auto result{std::to_chars(error_text, error_text + sizeof(error_text), error)};
    throw std::range_error(std::string{operation} + " failed with GetLastError()=" + std::string{error_text, result.ptr});
}

template <typename Input>
void CheckInputSize(Input in)
{
    if (in.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::range_error("UTF conversion input length exceeds INT_MAX");
    }
}

} // namespace

std::wstring Utf8ToUtf16(std::string_view in)
{
    if (in.empty()) return {};
    CheckInputSize(in);

    const int input_size{static_cast<int>(in.size())};
    const int output_size{MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, in.data(), input_size, nullptr, 0)};
    if (output_size == 0) ThrowConversionError("MultiByteToWideChar size query", GetLastError());

    std::wstring out(output_size, L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, in.data(), input_size, out.data(), output_size) == 0) {
        ThrowConversionError("MultiByteToWideChar conversion", GetLastError());
    }
    return out;
}

std::string Utf16ToUtf8(std::wstring_view in)
{
    if (in.empty()) return {};
    CheckInputSize(in);

    const int input_size{static_cast<int>(in.size())};
    const int output_size{WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, in.data(), input_size, nullptr, 0, nullptr, nullptr)};
    if (output_size == 0) ThrowConversionError("WideCharToMultiByte size query", GetLastError());

    std::string out(output_size, '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, in.data(), input_size, out.data(), output_size, nullptr, nullptr) == 0) {
        ThrowConversionError("WideCharToMultiByte conversion", GetLastError());
    }
    return out;
}

} // namespace util

#endif // WIN32
