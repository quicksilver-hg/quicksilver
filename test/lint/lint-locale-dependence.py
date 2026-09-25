#!/usr/bin/env python3
# Copyright (c) 2018-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# Be aware that quicksilver-daemon and quicksilver differ in terms of localization: Qt
# opts in to POSIX localization by running setlocale(LC_ALL, "") on startup,
# whereas no such call is made in quicksilver-daemon.
#
# Qt runs setlocale(LC_ALL, "") on initialization. This installs the locale
# specified by the user's LC_ALL (or LC_*) environment variable as the new
# C locale.
#
# In contrast, quicksilver-daemon does not opt in to localization -- no call to
# setlocale(LC_ALL, "") is made and the environment variables LC_* are
# thus ignored.
#
# This results in situations where quicksilver-daemon is guaranteed to be running
# with the classic locale ("C") whereas the locale of quicksilver will vary
# depending on the user's environment variables.
#
# An example: Assuming the environment variable LC_ALL=de_DE then the
# call std::to_string(1.23) will return "1.230000" in quicksilver-daemon but
# "1,230000" in quicksilver.
#
# From the Qt documentation:
# "On Unix/Linux Qt is configured to use the system locale settings by default.
#  This can cause a conflict when using POSIX functions, for instance, when
#  converting between data types such as floats and strings, since the notation
#  may differ between locales. To get around this problem, call the POSIX function
#  setlocale(LC_NUMERIC,"C") right after initializing QApplication, QGuiApplication
#  or QCoreApplication to reset the locale that is used for number formatting to
#  "C"-locale."
#
# See https://doc.qt.io/qt-5/qcoreapplication.html#locale-settings and
# https://stackoverflow.com/a/34878283 for more details.

import re
import sys

from subprocess import check_output, CalledProcessError


KNOWN_VIOLATIONS = [
    "src/crypto/cuckatoo/gpu_solver.cpp:.*std::strtol",
    "src/crypto/cuckatoo/gpu_solver.cpp:.*std::strtoul",
    "src/crypto/cuckatoo/gpu_solver.cpp:.*std::strtoull",
    "src/crypto/cuckatoo/gpu_solver.cpp:.*std::to_string",
    "src/crypto/cuckatoo/vendor/cuckatoo.h:.*vprintf",
    "src/crypto/cuckatoo/vendor/lean.cpp:.*atoi",
    "src/crypto/cuckatoo/vendor/lean.cpp:.*sscanf",
    "src/dbwrapper.cpp:.*vsnprintf",
    "src/test/gpu_solver_tests.cpp:.*std::snprintf",
    # Bench-only calibration tool. Its stdout (the CSV it exists to produce) is
    # built locale-independently via util::ToString; these are stderr diagnostics
    # with %s/%u/%zu only, so no LC_NUMERIC surface. lint-cuckatoo-source-policy
    # forbids iostream and stringstream under src/crypto/cuckatoo/.
    "src/crypto/cuckatoo/bench/qscalibrate.cpp:.*std::fprintf",
    # --- CUDA sources, in scope since 2026-08-25 (F-35) -----------------------
    # The glob was "*.cpp" and "*.h" only, so nothing under a .cu/.cuh extension
    # had ever been examined. Widening it surfaced one real defect --
    # qsgpucalibrate printed its CSV "seconds" column with %.6f while its CPU
    # twin qscalibrate.cpp deliberately avoids that via Seconds6() -- which is
    # now fixed rather than exempted. Everything below is a genuine exemption.
    #
    # The three GPU tools are standalone nvcc binaries. They are never linked
    # into quicksilver-daemon or quicksilver and never call setlocale, so they run
    # in the classic "C" locale by construction, which is the same guarantee the
    # header comment above says quicksilver-daemon relies on. Their stdout contracts
    # (nonce=/cycle=, and the CSV) now use only %u/%s/%llx plus Seconds6, so no
    # LC_NUMERIC surface remains; the entries below are stderr diagnostics,
    # argument parsing, and integer formatting.
    "src/crypto/cuckatoo/gpu/qsgpusolve.cu:.*std::fprintf",
    "src/crypto/cuckatoo/gpu/qsgpusolve.cu:.*std::printf",
    "src/crypto/cuckatoo/gpu/qsgpusolve.cu:.*std::atoi",
    "src/crypto/cuckatoo/gpu/qsgpusolve.cu:.*std::sscanf",
    "src/crypto/cuckatoo/gpu/qsgpusolve.cu:.*std::strtoul",
    "src/crypto/cuckatoo/gpu/qsgpucalibrate.cu:.*std::fprintf",
    "src/crypto/cuckatoo/gpu/qsgpucalibrate.cu:.*std::printf",
    "src/crypto/cuckatoo/gpu/qsgpucalibrate.cu:.*std::snprintf",
    "src/crypto/cuckatoo/gpu/qsgpucalibrate.cu:.*std::strtoul",
    "src/crypto/cuckatoo/gpu/device_health.cuh:.*std::fprintf",
    # This matches PROSE, not code: the comment explains why edgetrimmer::trim()
    # is a name collision, and saying so requires naming it. Anchored to a
    # comment marker so a real boost trim in the file is still caught -- verified
    # by mutation. The sibling case (a comment naming the locale-install call in
    # qsgpucalibrate) needed no entry: lint-cuckatoo-source-policy.py bans that
    # token in cuckatoo sources outright, so the comment was reworded instead of
    # exempted twice.
    "src/crypto/cuckatoo/gpu/device_health.cuh:\\s*//.*trim",
    # Vendored upstream CUDA solver, same treatment as vendor/lean.cpp and
    # vendor/cuckatoo.h above. The atoi/sscanf calls are in its own main(), which
    # we compile out with CUCKATOO_NO_MAIN; the snprintf is gpuAssert's error
    # buffer. "trim" is a name collision with edgetrimmer::trim(), not the
    # locale-dependent boost algorithm.
    "src/crypto/cuckatoo/vendor/lean.cu:.*atoi",
    "src/crypto/cuckatoo/vendor/lean.cu:.*sscanf",
    "src/crypto/cuckatoo/vendor/lean.cu:.*snprintf",
    "src/crypto/cuckatoo/vendor/lean.cu:.*trim",
    # --------------------------------------------------------------------------
    "src/test/fuzz/locale.cpp:.*setlocale",
    "src/test/util_tests.cpp:.*strtoll",
    "src/util/syserror.cpp:.*strerror",      # Outside this function use `SysErrorString`
]

REGEXP_EXTERNAL_DEPENDENCIES_EXCLUSIONS = [
    "src/crypto/ctaes/",
    "src/leveldb/",
    "src/secp256k1/",
    "src/tinyformat.h",
    "research/tier0-pow/",
]

LOCALE_DEPENDENT_FUNCTIONS = [
    "alphasort",    # LC_COLLATE (via strcoll)
    "asctime",      # LC_TIME (directly)
    "asprintf",     # (via vasprintf)
    "atof",         # LC_NUMERIC (via strtod)
    "atoi",         # LC_NUMERIC (via strtol)
    "atol",         # LC_NUMERIC (via strtol)
    "atoll",        # (via strtoll)
    "atoq",
    "btowc",        # LC_CTYPE (directly)
    "ctime",        # (via asctime or localtime)
    "dprintf",      # (via vdprintf)
    "fgetwc",
    "fgetws",
    "fold_case",    # boost::locale::fold_case
    "fprintf",      # (via vfprintf)
    "fputwc",
    "fputws",
    "fscanf",       # (via __vfscanf)
    "fwprintf",     # (via __vfwprintf)
    "getdate",      # via __getdate_r => isspace // __localtime_r
    "getwc",
    "getwchar",
    "is_digit",     # boost::algorithm::is_digit
    "is_space",     # boost::algorithm::is_space
    "isalnum",      # LC_CTYPE
    "isalpha",      # LC_CTYPE
    "isblank",      # LC_CTYPE
    "iscntrl",      # LC_CTYPE
    "isctype",      # LC_CTYPE
    "isdigit",      # LC_CTYPE
    "isgraph",      # LC_CTYPE
    "islower",      # LC_CTYPE
    "isprint",      # LC_CTYPE
    "ispunct",      # LC_CTYPE
    "isspace",      # LC_CTYPE
    "isupper",      # LC_CTYPE
    "iswalnum",     # LC_CTYPE
    "iswalpha",     # LC_CTYPE
    "iswblank",     # LC_CTYPE
    "iswcntrl",     # LC_CTYPE
    "iswctype",     # LC_CTYPE
    "iswdigit",     # LC_CTYPE
    "iswgraph",     # LC_CTYPE
    "iswlower",     # LC_CTYPE
    "iswprint",     # LC_CTYPE
    "iswpunct",     # LC_CTYPE
    "iswspace",     # LC_CTYPE
    "iswupper",     # LC_CTYPE
    "iswxdigit",    # LC_CTYPE
    "isxdigit",     # LC_CTYPE
    "localeconv",   # LC_NUMERIC + LC_MONETARY
    "mblen",        # LC_CTYPE
    "mbrlen",
    "mbrtowc",
    "mbsinit",
    "mbsnrtowcs",
    "mbsrtowcs",
    "mbstowcs",     # LC_CTYPE
    "mbtowc",       # LC_CTYPE
    "mktime",
    "normalize",    # boost::locale::normalize
    "printf",       # LC_NUMERIC
    "putwc",
    "putwchar",
    "scanf",        # LC_NUMERIC
    "setlocale",
    "snprintf",
    "sprintf",
    "sscanf",
    "std::locale::global",
    "std::to_string",
    "stod",
    "stof",
    "stoi",
    "stol",
    "stold",
    "stoll",
    "stoul",
    "stoull",
    "strcasecmp",
    "strcasestr",
    "strcoll",      # LC_COLLATE
    "strerror",
    "strfmon",
    "strftime",     # LC_TIME
    "strncasecmp",
    "strptime",
    "strtod",       # LC_NUMERIC
    "strtof",
    "strtoimax",
    "strtol",       # LC_NUMERIC
    "strtold",
    "strtoll",
    "strtoq",
    "strtoul",      # LC_NUMERIC
    "strtoull",
    "strtoumax",
    "strtouq",
    "strxfrm",      # LC_COLLATE
    "swprintf",
    "to_lower",     # boost::locale::to_lower
    "to_title",     # boost::locale::to_title
    "to_upper",     # boost::locale::to_upper
    "tolower",      # LC_CTYPE
    "toupper",      # LC_CTYPE
    "towctrans",
    "towlower",     # LC_CTYPE
    "towupper",     # LC_CTYPE
    "trim",         # boost::algorithm::trim
    "trim_left",    # boost::algorithm::trim_left
    "trim_right",   # boost::algorithm::trim_right
    "ungetwc",
    "vasprintf",
    "vdprintf",
    "versionsort",
    "vfprintf",
    "vfscanf",
    "vfwprintf",
    "vprintf",
    "vscanf",
    "vsnprintf",
    "vsprintf",
    "vsscanf",
    "vswprintf",
    "vwprintf",
    "wcrtomb",
    "wcscasecmp",
    "wcscoll",      # LC_COLLATE
    "wcsftime",     # LC_TIME
    "wcsncasecmp",
    "wcsnrtombs",
    "wcsrtombs",
    "wcstod",       # LC_NUMERIC
    "wcstof",
    "wcstoimax",
    "wcstol",       # LC_NUMERIC
    "wcstold",
    "wcstoll",
    "wcstombs",     # LC_CTYPE
    "wcstoul",      # LC_NUMERIC
    "wcstoull",
    "wcstoumax",
    "wcswidth",
    "wcsxfrm",      # LC_COLLATE
    "wctob",
    "wctomb",       # LC_CTYPE
    "wctrans",
    "wctype",
    "wcwidth",
    "wprintf"
]


def find_locale_dependent_function_uses():
    regexp_locale_dependent_functions = "|".join(LOCALE_DEPENDENT_FUNCTIONS)
    exclude_args = [":(exclude)" + excl for excl in REGEXP_EXTERNAL_DEPENDENCIES_EXCLUSIONS]
    git_grep_command = ["git", "grep", "-E", "[^a-zA-Z0-9_\\`'\"<>](" +  regexp_locale_dependent_functions + ")(_r|_s)?[^a-zA-Z0-9_\\`'\"<>]", "--", "*.cpp", "*.h", "*.cu", "*.cuh"] + exclude_args
    git_grep_output = list()

    try:
        git_grep_output = check_output(git_grep_command, text=True, encoding="utf8").splitlines()
    except CalledProcessError as e:
        if e.returncode > 1:
            raise e

    return git_grep_output


def main():
    exit_code = 0

    regexp_ignore_known_violations = "|".join(KNOWN_VIOLATIONS)
    git_grep_output = find_locale_dependent_function_uses()

    for locale_dependent_function in LOCALE_DEPENDENT_FUNCTIONS:
        matches =  [line for line in git_grep_output
                    if re.search("[^a-zA-Z0-9_\\`'\"<>]" + locale_dependent_function + "(_r|_s)?[^a-zA-Z0-9_\\`'\"<>]", line)
                    and not re.search("\\.(c|cpp|h):\\s*(//|\\*|/\\*|\").*" + locale_dependent_function, line)
                    and not re.search(regexp_ignore_known_violations, line)]
        if matches:
            print(f"The locale dependent function {locale_dependent_function}(...) appears to be used:")
            for match in matches:
                print(match)
            print("")
            exit_code = 1

    if exit_code == 1:
        print("Unnecessary locale dependence can cause bugs that are very tricky to isolate and fix. Please avoid using locale-dependent functions if possible.\n")
        print(f"Advice not applicable in this specific case? Add an exception by updating the ignore list in {sys.argv[0]}")

    sys.exit(exit_code)


if __name__ == "__main__":
    main()
