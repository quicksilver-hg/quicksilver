#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Check that the generated documentation still matches the binaries.

doc/man/*.1 and share/examples/quicksilver.conf are generated from --help
output by contrib/devtools/gen-manpages.py and
contrib/devtools/gen-quicksilver-conf.sh. Nothing used to compare them against
the binaries, so an option could ship undocumented indefinitely, and a
generated file could be hand-edited without anything noticing. Both happened:

  * -headerstore, -paymentreceiptdir and -receiptstore were added without the
    generator ever being run, and the omission only surfaced when an unrelated
    change regenerated the pages.
  * e59f7070 renamed "raw scriptPubKey" to "output script" and edited the
    example conf in place instead of regenerating it, keeping the old
    line-wrap point. The file stopped matching its generator.

This test is the fence. It lives in the functional suite rather than
test/lint/ because it needs built binaries, and the lint runner deliberately
runs against source alone -- a build-dependent check bolted in there would
have to skip when the binaries are absent, which is the failure mode this is
meant to remove.

The man-page half compares option *sets* rather than regenerating and
diffing. That is deliberate: the pages embed the commit they were generated
from, which is necessarily an ancestor of the commit that contains them, so a
byte-for-byte comparison against a fresh generation could never pass. It also
keeps help2man off the test's dependency list. The property F-14 actually
names -- an option shipping undocumented -- is a set comparison, and it is
asserted in both directions so a stale entry for a removed option fails too.

The conf half is a byte comparison, because that generator only prefixes
--help output with "# " and embeds no version string.

An ENABLE_EXTERNAL_SIGNER build is allowed to differ by exactly the options
registered inside that ifdef (today: -signer) and by nothing else. The man
pages and the example conf are generated from the shipping OFF build, so
those options are absent there on purpose. The allowed set is a literal,
checked against the source, so adding another guarded option fails here
instead of being absorbed.
"""

import os
import platform
import re
import shutil
import subprocess
import tempfile

from test_framework.test_framework import QuicksilverTestFramework
from test_framework.util import assert_equal, resolve_binary_path

# Options are indented two spaces in --help output; the name runs to the first
# "=" or whitespace.
HELP_OPTION_RE = re.compile(r"^  -([a-zA-Z0-9_.-]+)")

# In roff, help2man writes an option definition as the line after ".HP", and
# escapes *every* hyphen: -reindex-chainstate becomes \fB\-reindex\-chainstate\fR.
# Anchoring on .HP is what separates a definition from the same option merely
# mentioned inside another option's description.
MAN_OPTION_RE = re.compile(r"^\\fB\\-((?:\\-|[a-zA-Z0-9_.])+)")

# Options registered only inside `#ifdef ENABLE_EXTERNAL_SIGNER`. Kept as a
# literal so a new guarded AddArg fails this test instead of widening it.
# The source is parsed and must name this same set (see signer_addarg_options).
SIGNER_ONLY_OPTIONS = frozenset({"signer"})

# AddVaultOptions registers this with no ifdef. A binary whose help or man
# page contains it is one that gains exactly SIGNER_ONLY_OPTIONS when the
# external signer is compiled in.
VAULT_OPTIONS_MARK = "vaultdir"

_ADDARG_RE = re.compile(r'AddArg\(\s*"-([A-Za-z0-9_.-]+)')
_PREPROC_RE = re.compile(r"^\s*#\s*(ifdef|ifndef|if|else|elif|endif)\b(.*)$")


def signer_addarg_options(srcdir):
    """Option names passed to AddArg inside `#ifdef ENABLE_EXTERNAL_SIGNER`."""
    found = set()
    src = os.path.join(srcdir, "src")
    for root, dirs, files in os.walk(src):
        for name in files:
            if name.endswith((".cpp", ".h", ".c", ".hpp")):
                found |= _addargs_in_signer_region(os.path.join(root, name))
    return found


def _signer_frame(kind, rest):
    text = rest.strip()
    if kind == "ifdef" and text == "ENABLE_EXTERNAL_SIGNER":
        return "signer-on"
    if kind == "ifndef" and text == "ENABLE_EXTERNAL_SIGNER":
        return "signer-off"
    if kind == "if" and text in ("defined(ENABLE_EXTERNAL_SIGNER)", "defined (ENABLE_EXTERNAL_SIGNER)"):
        return "signer-on"
    if kind == "if" and text in ("!defined(ENABLE_EXTERNAL_SIGNER)", "!defined (ENABLE_EXTERNAL_SIGNER)"):
        return "signer-off"
    return "other"


def _addargs_in_signer_region(path):
    found = set()
    stack = []
    with open(path, encoding="utf8") as source:
        for line in source:
            match = _PREPROC_RE.match(line)
            if match:
                kind, rest = match.group(1), match.group(2)
                if kind in ("ifdef", "ifndef", "if"):
                    stack.append(_signer_frame(kind, rest))
                elif kind == "else" and stack:
                    top = stack[-1]
                    if top == "signer-on":
                        stack[-1] = "signer-off"
                    elif top == "signer-off":
                        stack[-1] = "signer-on"
                elif kind == "elif" and stack and stack[-1] in ("signer-on", "signer-off"):
                    stack[-1] = "signer-off"
                elif kind == "endif" and stack:
                    stack.pop()
                continue
            if "signer-on" in stack:
                for name in _ADDARG_RE.findall(line):
                    found.add(name)
    return found


def _conf_option_names(text):
    names = set()
    for line in text.splitlines():
        match = re.match(r"^#([A-Za-z0-9_.-]+)", line)
        if match:
            names.add(match.group(1))
    return names


def _strip_conf_options(text, names):
    """Drop the generator's blocks for `names` and return (text, names removed).

    gen-quicksilver-conf.sh prints an option's description, then the option,
    then a blank line. Removing that triple leaves the OFF-build conf.
    """
    lines = text.splitlines()
    drop = set()
    removed = set()
    for index, line in enumerate(lines):
        match = re.match(r"^#([A-Za-z0-9_.-]+)", line)
        if not match or match.group(1) not in names:
            continue
        removed.add(match.group(1))
        drop.add(index)
        cursor = index - 1
        while cursor >= 0 and lines[cursor].startswith("# "):
            drop.add(cursor)
            cursor -= 1
        if index + 1 < len(lines) and lines[index + 1] == "":
            drop.add(index + 1)
    kept = [line for index, line in enumerate(lines) if index not in drop]
    trailer = "\n" if text.endswith("\n") else ""
    return "\n".join(kept) + trailer, removed


# Every binary gen-manpages.py generates a page for.
DOCUMENTED_BINARIES = [
    "quicksilver-daemon",
    "quicksilver-cli",
    "quicksilver-tx",
    "quicksilver-vault",
    "quicksilver",
    "quicksilver-agent",
]


class GeneratedDocsTest(QuicksilverTestFramework):
    def set_test_params(self):
        self.num_nodes = 0
        # The binaries are invoked directly with --help: no chain, no cache, no
        # network. Without setup_clean_chain the framework builds the block
        # cache, which generates against self.nodes[0] and has no node here.
        self.setup_clean_chain = True

    def setup_network(self):
        pass

    def binary_path(self, name):
        return resolve_binary_path(
            self.config["environment"]["BUILDDIR"],
            name,
            self.config["environment"]["EXEEXT"],
        )

    def srcdir_path(self, *parts):
        return os.path.join(self.config["environment"]["SRCDIR"], *parts)

    def help_options(self, binary):
        # quicksilver would otherwise need a display; the help text is
        # identical either way. Verified width-independent at COLUMNS=40, 80
        # and 200, so this output is deterministic.
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
        result = subprocess.run(
            [binary, "--help"], capture_output=True, text=True, env=env, check=True
        )
        return {
            match.group(1)
            for line in result.stdout.splitlines()
            if (match := HELP_OPTION_RE.match(line))
        }

    def man_options(self, path):
        with open(path, encoding="utf8") as man_page:
            lines = man_page.read().splitlines()
        options = set()
        for index, line in enumerate(lines[:-1]):
            if line.strip() != ".HP":
                continue
            match = MAN_OPTION_RE.match(lines[index + 1])
            if match:
                options.add(match.group(1).replace("\\-", "-"))
        return options

    def assert_signer_source(self):
        parsed = signer_addarg_options(self.config["environment"]["SRCDIR"])
        unexpected = sorted(parsed - SIGNER_ONLY_OPTIONS)
        missing = sorted(SIGNER_ONLY_OPTIONS - parsed)
        assert parsed == SIGNER_ONLY_OPTIONS, (
            "ENABLE_EXTERNAL_SIGNER AddArg options are "
            f"{sorted(parsed)}, expected {sorted(SIGNER_ONLY_OPTIONS)}. "
            f"Unexpected: {unexpected}. Missing: {missing}."
        )

    def check_man_pages(self):
        # Read straight from the build's component list. is_external_signer_compiled()
        # is this same lookup; kept inline so the comparison shows which config it uses.
        signer_on = self.config["components"].getboolean("ENABLE_EXTERNAL_SIGNER")
        if platform.system() == "Windows":
            # The checked-in pages are generated from a POSIX build and describe
            # one. -daemon and -daemonwait are registered only #if HAVE_DECL_FORK
            # (src/init.cpp), so on Windows they are hidden args and never reach
            # --help, and the set comparison below reports them as stale. They are
            # not stale: regenerating on Windows would only move the failure to
            # every other platform. This half of the test compares one platform's
            # documentation against another platform's binary, which is not a
            # property the project holds. The conf half below still runs.
            self.log.warning(
                "doc/man/*.1 describes a POSIX build; man pages are NOT verified on Windows"
            )
            return

        checked = 0
        for name in DOCUMENTED_BINARIES:
            binary = self.binary_path(name)
            if not os.path.isfile(binary):
                # quicksilver is the only one that can legitimately be
                # absent, and only when the GUI was not built. Say so loudly:
                # its page is still checked in and goes unverified here.
                self.log.warning(
                    f"{name} was not built; doc/man/{name}.1 is NOT verified by this run"
                )
                continue

            page = self.srcdir_path("doc", "man", f"{name}.1")
            from_help = self.help_options(binary)
            from_man = self.man_options(page)

            # A page that parses to nothing would make the comparison below
            # pass vacuously against a binary with no options.
            assert from_man, f"parsed no options out of doc/man/{name}.1"

            extra = from_help - from_man
            stale = sorted(from_man - from_help)
            has_vault_options = (
                VAULT_OPTIONS_MARK in from_help or VAULT_OPTIONS_MARK in from_man
            )
            if signer_on and has_vault_options:
                # Stricter than the OFF check: the only permitted difference is
                # the signer-only set, in the help-minus-man direction. A second
                # undocumented option still fails, and so does losing -signer.
                unexpected = sorted(extra - SIGNER_ONLY_OPTIONS)
                lost = sorted(SIGNER_ONLY_OPTIONS - extra)
                assert not unexpected and not lost and not stale, (
                    f"{name}: an external-signer build may differ from "
                    f"doc/man/{name}.1 by exactly {sorted(SIGNER_ONLY_OPTIONS)} "
                    f"and nothing else. unexpected={unexpected} lost={lost} "
                    f"stale={stale}."
                )
            else:
                undocumented = sorted(extra)
                assert not undocumented, (
                    f"{name} accepts options that doc/man/{name}.1 does not document: "
                    f"{undocumented}. Rebuild, then run contrib/devtools/gen-manpages.py."
                )
                assert not stale, (
                    f"doc/man/{name}.1 documents options {name} no longer accepts: "
                    f"{stale}. Rebuild, then run contrib/devtools/gen-manpages.py."
                )
            self.log.info(f"{name}: {len(from_help)} options match doc/man/{name}.1")
            checked += 1

        # quicksilver-daemon, quicksilver-cli, quicksilver-tx and quicksilver-vault
        # are always built alongside the functional suite.
        # quicksilver and quicksilver-agent are checked when they were
        # built and skipped, with a warning, when they were not. Anything
        # less than five means this test checked less than it claims to.
        assert checked >= 5, f"only {checked} man pages were checked, expected at least 5"

    def usable_bash(self):
        """Return a bash that actually runs, or None.

        shutil.which("bash") is a presence probe, and presence is not the
        question. On a Windows runner it finds C:\\Windows\\System32\\bash.exe --
        the WSL launcher, which exists on every modern Windows and exits
        non-zero with "Windows Subsystem for Linux has no installed
        distributions" the moment it is asked to run anything. The guard below
        therefore passed and the generator then failed, which reads as a broken
        generator rather than a missing interpreter. Run the interpreter to find
        out whether it interprets.
        """
        bash = shutil.which("bash")
        if bash is None:
            return None
        try:
            probe = subprocess.run(
                [bash, "-c", "printf ok"],
                capture_output=True, text=True, timeout=60,
            )
        except (OSError, subprocess.SubprocessError):
            return None
        return bash if probe.returncode == 0 and probe.stdout.strip() == "ok" else None

    def check_example_conf(self):
        signer_on = self.config["components"].getboolean("ENABLE_EXTERNAL_SIGNER")
        if platform.system() == "Windows":
            # Same reason check_man_pages() skips: the committed conf is
            # generated from quicksilver-daemon --help on a POSIX build, and -daemon /
            # -daemonwait are registered only #if HAVE_DECL_FORK (src/init.cpp),
            # so a Windows binary never prints them. The diff that produces is
            # the platform's, not a stale file -- regenerating here would only
            # move the failure to every other platform. The POSIX assertion
            # below keeps its full strength.
            self.log.warning(
                "share/examples/quicksilver.conf describes a POSIX build; "
                "it is NOT verified on Windows"
            )
            return

        generator = self.srcdir_path("contrib", "devtools", "gen-quicksilver-conf.sh")
        bash = self.usable_bash()
        if bash is None:
            self.log.warning(
                "no working bash; share/examples/quicksilver.conf is NOT verified by this run"
            )
            return

        committed = self.srcdir_path("share", "examples", "quicksilver.conf")
        with tempfile.TemporaryDirectory() as tmpdir:
            regenerated = os.path.join(tmpdir, "quicksilver.conf")
            subprocess.run(
                [bash, generator],
                check=True,
                capture_output=True,
                text=True,
                env=dict(
                    os.environ,
                    TOPDIR=self.config["environment"]["SRCDIR"],
                    QUICKSILVER_DAEMON=self.binary_path("quicksilver-daemon"),
                    SHARE_EXAMPLES_DIR=tmpdir,
                    EXAMPLE_CONF_FILE=regenerated,
                ),
            )
            with open(regenerated, encoding="utf8") as generated_file:
                generated = generated_file.read()

        with open(committed, encoding="utf8") as committed_file:
            on_disk = committed_file.read()

        if signer_on:
            # Measured against an ON --help: the generator emits -signer, so a
            # byte compare with the committed OFF conf fails on that option.
            # Strip exactly that block. Anything else still fails.
            generated_names = _conf_option_names(generated)
            disk_names = _conf_option_names(on_disk)
            unexpected = sorted(generated_names - disk_names - SIGNER_ONLY_OPTIONS)
            lost = sorted(SIGNER_ONLY_OPTIONS - generated_names)
            assert not unexpected and not lost, (
                "share/examples/quicksilver.conf under ENABLE_EXTERNAL_SIGNER "
                f"may add exactly {sorted(SIGNER_ONLY_OPTIONS)}. "
                f"unexpected={unexpected} lost={lost}."
            )
            generated, removed = _strip_conf_options(generated, SIGNER_ONLY_OPTIONS)
            assert removed == set(SIGNER_ONLY_OPTIONS), (
                "failed to isolate the signer block in the generated conf: "
                f"removed={sorted(removed)}"
            )

        if generated != on_disk:
            # Point at the first differing line; the whole file is 21kB.
            generated_lines = generated.splitlines()
            on_disk_lines = on_disk.splitlines()
            for number, (want, have) in enumerate(zip(generated_lines, on_disk_lines), 1):
                if want != have:
                    raise AssertionError(
                        "share/examples/quicksilver.conf does not match its generator, "
                        f"first difference at line {number}:\n"
                        f"  generated: {want!r}\n"
                        f"  on disk:   {have!r}\n"
                        "Rebuild, then run contrib/devtools/gen-quicksilver-conf.sh."
                    )
            raise AssertionError(
                "share/examples/quicksilver.conf does not match its generator: "
                f"{len(generated_lines)} generated lines vs {len(on_disk_lines)} on disk. "
                "Rebuild, then run contrib/devtools/gen-quicksilver-conf.sh."
            )
        assert_equal(generated, on_disk)
        self.log.info("share/examples/quicksilver.conf matches its generator")

    def run_test(self):
        self.assert_signer_source()
        self.log.info("Check doc/man/*.1 against each binary's --help")
        self.check_man_pages()

        self.log.info("Check share/examples/quicksilver.conf against its generator")
        self.check_example_conf()


if __name__ == "__main__":
    GeneratedDocsTest(__file__).main()
