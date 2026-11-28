// Copyright (c) 2024-present The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/string.h>

#include <boost/test/unit_test.hpp>
#include <test/util/setup_common.h>

#include <ios>
#include <limits>
#include <sstream>

using namespace util;
using util::detail::CheckNumFormatSpecifiers;

BOOST_AUTO_TEST_SUITE(util_string_tests)

BOOST_AUTO_TEST_CASE(tinyformat_range_and_stream_state)
{
    const std::string too_wide{"%2147483648d"};
    BOOST_CHECK_THROW(tfm::format(tfm::RuntimeFormat{too_wide}, 1), tfm::format_error);
    BOOST_CHECK_THROW(tfm::format(ConstevalFormatString<2>{"%*d"}, std::numeric_limits<int>::min(), 1), tfm::format_error);
    BOOST_CHECK_THROW(tfm::format(ConstevalFormatString<2>{"%*d"}, 1e100, 1), tfm::format_error);
    BOOST_CHECK_THROW(tfm::format(ConstevalFormatString<1>{"%c"}, 1e100), tfm::format_error);

    std::ostringstream out;
    out.width(7);
    out.precision(3);
    out.fill('_');
    out.setf(std::ios::hex, std::ios::basefield);
    const auto flags = out.flags();
    BOOST_CHECK_THROW(tfm::format(out, ConstevalFormatString<1>{"%08n"}, 0), tfm::format_error);
    BOOST_CHECK_EQUAL(out.width(), 7);
    BOOST_CHECK_EQUAL(out.precision(), 3);
    BOOST_CHECK_EQUAL(out.fill(), '_');
    BOOST_CHECK(out.flags() == flags);
}

template <unsigned NumArgs>
void TfmFormatZeroes(const std::string& fmt)
{
    std::apply([&](auto... args) {
        (void)tfm::format(tfm::RuntimeFormat{fmt}, args...);
    }, std::array<int, NumArgs>{});
}

// Helper to allow compile-time sanity checks while providing the number of
// args directly. Normally PassFmt<sizeof...(Args)> would be used.
template <unsigned NumArgs>
void PassFmt(ConstevalFormatString<NumArgs> fmt)
{
    // Execute compile-time check again at run-time to get code coverage stats
    BOOST_CHECK_NO_THROW(CheckNumFormatSpecifiers<NumArgs>(fmt.fmt));

    // If ConstevalFormatString didn't throw above, make sure tinyformat doesn't
    // throw either for the same format string and parameter count combination.
    // Proves that we have some extent of protection from runtime errors
    // (tinyformat may still throw for some type mismatches).
    BOOST_CHECK_NO_THROW(TfmFormatZeroes<NumArgs>(fmt.fmt));
}
template <unsigned WrongNumArgs>
void FailFmtWithError(const char* wrong_fmt, std::string_view error)
{
    BOOST_CHECK_EXCEPTION(CheckNumFormatSpecifiers<WrongNumArgs>(wrong_fmt), const char*, HasReason{error});
}

BOOST_AUTO_TEST_CASE(ConstevalFormatString_NumSpec)
{
    PassFmt<0>("");
    PassFmt<0>("%%");
    PassFmt<1>("%s");
    PassFmt<1>("%c");
    PassFmt<0>("%%s");
    PassFmt<0>("s%%");
    PassFmt<1>("%%%s");
    PassFmt<1>("%s%%");
    PassFmt<0>(" 1$s");
    PassFmt<1>("%1$s");
    PassFmt<1>("%1$s%1$s");
    PassFmt<2>("%2$s");
    PassFmt<2>("%2$s 4$s %2$s");
    PassFmt<129>("%129$s 999$s %2$s");
    PassFmt<1>("%02d");
    PassFmt<1>("%+2s");
    PassFmt<1>("%.6i");
    PassFmt<1>("%5.2f");
    PassFmt<1>("%5.f");
    PassFmt<1>("%.f");
    PassFmt<1>("%#x");
    PassFmt<1>("%1$5i");
    PassFmt<1>("%1$-5i");
    PassFmt<1>("%1$.5i");
    // tinyformat accepts almost any "type" spec, even '%', or '_', or '\n'.
    PassFmt<1>("%123%");
    PassFmt<1>("%123%s");
    PassFmt<1>("%_");
    PassFmt<1>("%\n");

    PassFmt<2>("%*c");
    PassFmt<2>("%+*c");
    PassFmt<2>("%.*f");
    PassFmt<3>("%*.*f");
    PassFmt<3>("%2$*3$d");
    PassFmt<3>("%2$*3$.9d");
    PassFmt<3>("%2$.*3$d");
    PassFmt<3>("%2$9.*3$d");
    PassFmt<3>("%2$+9.*3$d");
    PassFmt<4>("%3$*2$.*4$f");

    // Make sure multiple flag characters "- 0+" are accepted
    PassFmt<3>("'%- 0+*.*f'");
    PassFmt<3>("'%1$- 0+*3$.*2$f'");

    auto err_mix{"Format specifiers must be all positional or all non-positional!"};
    FailFmtWithError<1>("%s%1$s", err_mix);
    FailFmtWithError<2>("%2$*d", err_mix);
    FailFmtWithError<2>("%*2$d", err_mix);
    FailFmtWithError<2>("%.*3$d", err_mix);
    FailFmtWithError<2>("%2$.*d", err_mix);

    auto err_num{"Format specifier count must match the argument count!"};
    FailFmtWithError<1>("", err_num);
    FailFmtWithError<0>("%s", err_num);
    FailFmtWithError<2>("%s", err_num);
    FailFmtWithError<0>("%1$s", err_num);
    FailFmtWithError<2>("%1$s", err_num);
    FailFmtWithError<1>("%4294967297$s", err_num);
    FailFmtWithError<1>("%*c", err_num);

    auto err_0_pos{"Positional format specifier must have position of at least 1"};
    FailFmtWithError<1>("%$s", err_0_pos);
    FailFmtWithError<1>("%$", err_0_pos);
    FailFmtWithError<0>("%0$", err_0_pos);
    FailFmtWithError<0>("%0$s", err_0_pos);
    FailFmtWithError<2>("%2$*$d", err_0_pos);
    FailFmtWithError<2>("%2$*0$d", err_0_pos);
    FailFmtWithError<3>("%3$*2$.*$f", err_0_pos);
    FailFmtWithError<3>("%3$*2$.*0$f", err_0_pos);

    auto err_term{"Format specifier incorrectly terminated by end of string"};
    FailFmtWithError<1>("%", err_term);
    FailFmtWithError<1>("%9", err_term);
    FailFmtWithError<1>("%9.", err_term);
    FailFmtWithError<1>("%9.9", err_term);
    FailFmtWithError<1>("%*", err_term);
    FailFmtWithError<1>("%+*", err_term);
    FailFmtWithError<1>("%.*", err_term);
    FailFmtWithError<1>("%9.*", err_term);
    FailFmtWithError<1>("%1$", err_term);
    FailFmtWithError<1>("%1$9", err_term);
    FailFmtWithError<2>("%1$*2$", err_term);
    FailFmtWithError<2>("%1$.*2$", err_term);
    FailFmtWithError<2>("%1$9.*2$", err_term);

    // Non-parity between tinyformat and ConstevalFormatString.
    // tinyformat throws but ConstevalFormatString does not.
    BOOST_CHECK_EXCEPTION(tfm::format(ConstevalFormatString<1>{"%n"}, 0), tfm::format_error,
        HasReason{"tinyformat: %n conversion spec not supported"});
    BOOST_CHECK_EXCEPTION(tfm::format(ConstevalFormatString<2>{"%*s"}, "hi", "hi"), tfm::format_error,
        HasReason{"tinyformat: Cannot convert from argument type to integer for use as variable width or precision"});
    BOOST_CHECK_EXCEPTION(tfm::format(ConstevalFormatString<2>{"%.*s"}, "hi", "hi"), tfm::format_error,
        HasReason{"tinyformat: Cannot convert from argument type to integer for use as variable width or precision"});

    // Ensure that tinyformat throws if format string contains wrong number
    // of specifiers. PassFmt relies on this to verify tinyformat successfully
    // formats the strings, and will need to be updated if tinyformat is changed
    // not to throw on failure.
    BOOST_CHECK_EXCEPTION(TfmFormatZeroes<2>("%s"), tfm::format_error,
        HasReason{"tinyformat: Not enough conversion specifiers in format string"});
    BOOST_CHECK_EXCEPTION(TfmFormatZeroes<1>("%s %s"), tfm::format_error,
        HasReason{"tinyformat: Too many conversion specifiers in format string"});
}

BOOST_AUTO_TEST_SUITE_END()
