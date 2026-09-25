#!/usr/bin/env python3
#
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
#
# Check that user-facing desktop packaging keeps developer binaries separated.
#
# The boundary is: a release install places the application and the agent client
# on the user's system, and nothing else. quicksilver-agent moved from the
# developer side to the application side deliberately -- it is the artifact an
# agent runs, and agents are the audience, so it is not a development aid.

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]

DEVELOPER_BINS = {
    "quicksilver-daemon": "@QUICKSILVER_DAEMON_NAME@",
    "quicksilver-cli": "@QUICKSILVER_CLI_NAME@",
    "quicksilver-tx": "@QUICKSILVER_TX_NAME@",
    "quicksilver-vault": "@QUICKSILVER_VAULT_TOOL_NAME@",
}

# What a release install is allowed to place on the user's system.
APPLICATION_BINS = {
    "quicksilver": "@QUICKSILVER_GUI_NAME@",
    "quicksilver-agent": "@QUICKSILVER_AGENT_NAME@",
}

DEVTOOLS_INSTALL = "contrib/debian/quicksilver-devtools.install"

TEST_BINS = {
    "test_quicksilver",
    "test_quicksilver-qt",
    "bench_quicksilver",
}


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf8")


def debian_stanzas(control: str) -> dict[str, str]:
    stanzas = {}
    for stanza in re.split(r"\n\s*\n", control.strip()):
        match = re.search(r"^Package:\s*(\S+)\s*$", stanza, re.MULTILINE)
        if match:
            stanzas[match.group(1)] = stanza
    return stanzas


def check_debian(failures: list[str]) -> None:
    stanzas = debian_stanzas(read("contrib/debian/control"))
    qt_stanza = stanzas.get("quicksilver", "")
    devtools_stanza = stanzas.get("quicksilver-devtools", "")
    if not qt_stanza:
        failures.append("contrib/debian/control: missing quicksilver package stanza")
    elif re.search(r"^(?:Depends|Recommends|Pre-Depends):.*\bquicksilver-daemon\b", qt_stanza, re.MULTILINE):
        failures.append("contrib/debian/control: quicksilver must not pull in quicksilver-daemon")
    if not devtools_stanza:
        failures.append("contrib/debian/control: missing quicksilver-devtools package stanza")

    qt_install = set(read("contrib/debian/quicksilver.install").splitlines())
    for binary in set(DEVELOPER_BINS) | TEST_BINS:
        if f"usr/bin/{binary}" in qt_install:
            failures.append(f"contrib/debian/quicksilver.install: desktop package installs {binary}")
    for binary in APPLICATION_BINS:
        if f"usr/bin/{binary}" not in qt_install:
            failures.append(f"contrib/debian/quicksilver.install: application package omits {binary}")

    devtools_install = set(read(DEVTOOLS_INSTALL).splitlines())
    for binary in DEVELOPER_BINS:
        if f"usr/bin/{binary}" not in devtools_install:
            failures.append(f"{DEVTOOLS_INSTALL}: developer package omits {binary}")
    for binary in set(APPLICATION_BINS) | TEST_BINS:
        if f"usr/bin/{binary}" in devtools_install:
            failures.append(f"{DEVTOOLS_INSTALL}: developer package installs {binary}")

    # One file claimed by two packages is a dpkg conflict, so no manifest may
    # glob and no path may appear in both.
    for manifest, entries in (("contrib/debian/quicksilver.install", qt_install),
                              (DEVTOOLS_INSTALL, devtools_install)):
        for entry in entries:
            if "*" in entry or "?" in entry:
                failures.append(f"{manifest}: globbed entry {entry!r}; list files explicitly")
    for shared in qt_install & devtools_install:
        if shared.strip():
            failures.append(f"contrib/debian: {shared} is claimed by both packages")

    control = read("contrib/debian/control")
    source_stanza = control.split("\nPackage:", 1)[0]
    if not re.search(r"(?m)^\s+git,", source_stanza):
        failures.append(
            "contrib/debian/control: Build-Depends must include git so a "
            "package build can stamp quicksilver-build-info.h"
        )
    rules = read("contrib/debian/rules")
    if "QUICKSILVER_GENBUILD_NO_GIT" not in rules or "BUILD_GIT_(TAG|COMMIT)" not in rules:
        failures.append(
            "contrib/debian/rules: an unstamped package build must fail "
            "unless QUICKSILVER_GENBUILD_NO_GIT=1"
        )
    if "-DBUILD_AGENT=ON" not in rules:
        failures.append("contrib/debian/rules: packaging must explicitly build quicksilver-agent")
    if "-DQS_DEVELOPER_TOOLS=ON" not in rules:
        failures.append("contrib/debian/rules: must install developer tools so the split can claim them")

    readme = read("contrib/debian/README.md")
    for package_name in ("quicksilver", "quicksilver-devtools"):
        if f"`{package_name}`" not in readme:
            failures.append(f"contrib/debian/README.md: missing documented package `{package_name}`")
    if re.search(r"`quicksilver-daemon`\s*-", readme):
        failures.append("contrib/debian/README.md: quicksilver-daemon is a binary, not a Debian package")
    if "Install `quicksilver-daemon` separately" in readme:
        failures.append("contrib/debian/README.md: install quicksilver-devtools, not quicksilver-daemon")


def check_windows_installer(failures: list[str]) -> None:
    nsi = read("share/setup.nsi.in")
    if r"$INSTDIR\daemon" in nsi:
        failures.append("share/setup.nsi.in: developer tools must not use the legacy daemon folder")
    if r"$INSTDIR\developer-tools" not in nsi:
        failures.append("share/setup.nsi.in: missing developer-tools output directory")

    current_outpath = ""
    for line_number, raw_line in enumerate(nsi.splitlines(), start=1):
        line = raw_line.strip()
        if line.startswith("SetOutPath "):
            current_outpath = line.removeprefix("SetOutPath ").strip()
            continue
        if not line.startswith("File "):
            continue
        for binary, token in DEVELOPER_BINS.items():
            if binary in raw_line or token in raw_line:
                if current_outpath != r"$INSTDIR\developer-tools":
                    failures.append(
                        f"share/setup.nsi.in:{line_number}: {binary} must be under developer-tools"
                    )
        for binary, token in APPLICATION_BINS.items():
            if token in raw_line and current_outpath == r"$INSTDIR\developer-tools":
                failures.append(
                    f"share/setup.nsi.in:{line_number}: {binary} ships with the application, "
                    "not under developer-tools"
                )
        for binary in TEST_BINS:
            if binary in raw_line:
                failures.append(f"share/setup.nsi.in:{line_number}: installer must not ship {binary}")

    for binary, token in DEVELOPER_BINS.items():
        if token not in nsi:
            failures.append(f"share/setup.nsi.in: developer-tools section omits {binary}")
    for binary, token in APPLICATION_BINS.items():
        if token not in nsi:
            failures.append(f"share/setup.nsi.in: installer omits {binary}")

    # The developer tools must be a component the user can decline, not a
    # forced section. "/o" makes it unchecked by default.
    if not re.search(r'^\s*Section\s+/o\s+"Developer tools"', nsi, re.MULTILINE):
        failures.append(
            'share/setup.nsi.in: developer tools must be an optional Section /o "Developer tools"'
        )
    if "MUI_PAGE_COMPONENTS" not in nsi:
        failures.append("share/setup.nsi.in: no components page, so an optional section cannot be declined")
    if r"DisplayIcon $INSTDIR\@QUICKSILVER_GUI_NAME@@EXEEXT@" not in nsi:
        failures.append("share/setup.nsi.in: uninstall DisplayIcon must use the configured GUI binary name")

    maintenance = read("cmake/module/Maintenance.cmake")
    if "TARGET_FILE:test_quicksilver" in maintenance or "TARGET_FILE:bench_quicksilver" in maintenance:
        failures.append("cmake/module/Maintenance.cmake: deploy target must not stage test binaries")
    if "TARGET_FILE:quicksilver-agent" not in maintenance:
        failures.append("cmake/module/Maintenance.cmake: deploy target must stage quicksilver-agent with developer tools")

    generator = read("cmake/module/GenerateSetupNsi.cmake")
    if "QUICKSILVER_TEST_NAME" in generator:
        failures.append("cmake/module/GenerateSetupNsi.cmake: installer generator must not define test binary names")
    for binary, token in {**DEVELOPER_BINS, **APPLICATION_BINS}.items():
        if token.strip("@") not in generator:
            failures.append(f"cmake/module/GenerateSetupNsi.cmake: installer generator must define {binary}")


def check_public_desktop_metadata(failures: list[str]) -> None:
    desktop_path = "share/applications/io.github.quicksilver_hg.quicksilver.desktop"
    metainfo_path = "share/applications/io.github.quicksilver_hg.quicksilver.metainfo.xml"
    desktop = read(desktop_path)
    metainfo = read(metainfo_path)

    required_desktop_lines = {
        "Name=Quicksilver",
        "Exec=quicksilver %u",
        "Icon=quicksilver",
        "MimeType=x-scheme-handler/quicksilver;",
        "StartupWMClass=quicksilver",
    }
    for line in sorted(required_desktop_lines):
        if line not in desktop.splitlines():
            failures.append(f"{desktop_path}: missing `{line}`")

    required_metainfo_fragments = {
        "<id>io.github.quicksilver_hg.quicksilver</id>",
        "<launchable type=\"desktop-id\">io.github.quicksilver_hg.quicksilver.desktop</launchable>",
        "<mediatype>x-scheme-handler/quicksilver</mediatype>",
        "<url type=\"homepage\">https://github.com/quicksilver-hg/quicksilver</url>",
        "<url type=\"bugtracker\">https://github.com/quicksilver-hg/quicksilver/issues</url>",
        "<url type=\"vcs-browser\">https://github.com/quicksilver-hg/quicksilver</url>",
        "<developer id=\"io.github.quicksilver_hg\">",
        "<name>The Quicksilver developers</name>",
    }
    for fragment in sorted(required_metainfo_fragments):
        if fragment not in metainfo:
            failures.append(f"{metainfo_path}: missing `{fragment}`")

    forbidden_public_identity = re.compile(
        r"\b(?:bitcoin|bitcoins|btc|satoshi)\b|bitcoin(?::|://|-qt)",
        re.IGNORECASE,
    )
    for path, contents in ((desktop_path, desktop), (metainfo_path, metainfo)):
        for line_number, line in enumerate(contents.splitlines(), start=1):
            if forbidden_public_identity.search(line):
                failures.append(f"{path}:{line_number}: stale public identity `{line.strip()}`")


def main() -> int:
    failures: list[str] = []
    check_debian(failures)
    check_windows_installer(failures)
    check_public_desktop_metadata(failures)
    if failures:
        print("Desktop packaging boundary violations:")
        print("\n".join(failures))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
