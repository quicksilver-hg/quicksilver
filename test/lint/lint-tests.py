#!/usr/bin/env python3
#
# Copyright (c) 2018-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""
Check the test suite naming conventions
"""

import re
import subprocess
import sys


def grep_boost_fixture_test_suite():
    command = [
        "git",
        "grep",
        "-E",
        r"^BOOST_FIXTURE_TEST_SUITE\(",
        "--",
        "src/test/**.cpp",
        "src/vault/test/**.cpp",
        "src/qt/test/**.cpp",
    ]
    return subprocess.check_output(command, text=True, encoding="utf8")


def grep_qt_test_classes():
    command = [
        "git",
        "grep",
        "-E",
        r"^class \w+Tests : public QObject",
        "--",
        "src/qt/test/**.h",
    ]
    return subprocess.check_output(command, text=True, encoding="utf8")


def check_qt_class_matches_filename(class_list):
    """src/qt/test/uritests.h must declare class URITests (case-insensitive)."""
    not_matching = []
    for line in class_list:
        match = re.search(r"/([^/]+)\.h:class (\w+Tests) : public QObject", line)
        if match is None or match.group(1).lower() != match.group(2).lower():
            not_matching.append(line)
    if not_matching:
        print(
            "The Qt test class in file src/qt/test/footests.h should be named\n"
            "FooTests. Please make sure the following test classes follow\n"
            "that convention:\n\n"
            + "\n".join(not_matching)
            + "\n"
        )
        return 1
    return 0


def check_matching_test_names(test_suite_list):
    not_matching = [
        x
        for x in test_suite_list
        if re.search(r"/(.*?)\.cpp:BOOST_FIXTURE_TEST_SUITE\(\1, .*\)", x) is None
    ]
    if len(not_matching) > 0:
        not_matching = "\n".join(not_matching)
        error_msg = (
            "The test suite in file src/test/foo_tests.cpp should be named\n"
            '"foo_tests". Please make sure the following test suites follow\n'
            "that convention:\n\n"
            f"{not_matching}\n"
        )
        print(error_msg)
        return 1
    return 0


def get_duplicates(input_list):
    """
    From https://stackoverflow.com/a/9835819
    """
    seen = set()
    dupes = set()
    for x in input_list:
        if x in seen:
            dupes.add(x)
        else:
            seen.add(x)
    return dupes


def check_unique_test_names(test_suite_list):
    output = [re.search(r"\((.*?),", x) for x in test_suite_list]
    output = [x.group(1) for x in output if x is not None]
    output = get_duplicates(output)
    output = sorted(list(output))

    if len(output) > 0:
        output = "\n".join(output)
        error_msg = (
            "Test suite names must be unique. The following test suite names\n"
            f"appear to be used more than once:\n\n{output}"
        )
        print(error_msg)
        return 1
    return 0


def main():
    test_suite_list = grep_boost_fixture_test_suite().splitlines()
    qt_class_list = grep_qt_test_classes().splitlines()
    exit_code = check_matching_test_names(test_suite_list)
    exit_code |= check_unique_test_names(test_suite_list)
    exit_code |= check_qt_class_matches_filename(qt_class_list)
    qt_names = []
    for line in qt_class_list:
        match = re.search(r"class (\w+Tests) : public QObject", line)
        if match is not None:
            qt_names.append(match.group(1))
    exit_code |= check_unique_test_names([f"class ({name}," for name in qt_names])
    sys.exit(exit_code)


if __name__ == "__main__":
    main()
