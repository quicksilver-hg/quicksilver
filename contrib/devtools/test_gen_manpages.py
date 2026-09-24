#!/usr/bin/env python3
# Copyright (c) 2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

import importlib.util
from pathlib import Path
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("gen-manpages.py")
SPEC = importlib.util.spec_from_file_location("gen_manpages", SCRIPT)
assert SPEC is not None
GEN_MANPAGES = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(GEN_MANPAGES)


def page_text(name, title_version, name_version, body_version, see_also):
    escaped_body_version = body_version.replace("-", r"\-")
    return f"""\\." generated
.TH {name.upper()} "1" "September 2026" "{name} {title_version}" "User Commands"
.SH NAME
{name} \\- manual page for {name} {name_version}
.SH DESCRIPTION
{name} version {escaped_body_version}
.SH "SEE ALSO"
{see_also}
"""


class GenManpagesTest(unittest.TestCase):
    def test_project_release_with_rc(self):
        with tempfile.TemporaryDirectory() as directory:
            cmake = Path(directory) / "CMakeLists.txt"
            cmake.write_text(
                "set(CLIENT_VERSION_MAJOR 1)\n"
                "set(CLIENT_VERSION_MINOR 2)\n"
                "set(CLIENT_VERSION_BUILD 3)\n"
                "set(CLIENT_VERSION_RC 4)\n",
                encoding="utf8",
            )
            self.assertEqual(GEN_MANPAGES.project_release(cmake), "v1.2.3rc4")

    def test_rewrite_release_version_handles_roff_escaping(self):
        with tempfile.TemporaryDirectory() as directory:
            page = Path(directory) / "tool.1"
            page.write_text(
                "tool v0.1.1-0123456789ab\n" r"tool v0.1.1\-0123456789ab" + "\n",
                encoding="utf8",
            )
            GEN_MANPAGES.rewrite_release_version(page, "v0.1.1-0123456789ab", "v0.1.1")
            self.assertEqual(
                page.read_text(encoding="utf8"), "tool v0.1.1\ntool v0.1.1\n"
            )

    def test_consistency_uses_binary_list_for_every_page(self):
        names = [Path(path).name for path in GEN_MANPAGES.BINARIES]
        see_also = ", ".join(f"{name}(1)" for name in names)
        with tempfile.TemporaryDirectory() as directory:
            mandir = Path(directory)
            for name in names:
                (mandir / f"{name}.1").write_text(
                    page_text(name, "v0.1.1", "v0.1.1", "v0.1.1", see_also),
                    encoding="utf8",
                )
            self.assertTrue(GEN_MANPAGES.check_consistency(mandir))

            stale = mandir / f"{names[0]}.1"
            stale.write_text(
                page_text(names[0], "v0.1.0", "v0.1.0", "v0.1.0", see_also),
                encoding="utf8",
            )
            self.assertFalse(GEN_MANPAGES.check_consistency(mandir))


if __name__ == "__main__":
    unittest.main()
