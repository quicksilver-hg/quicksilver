#!/usr/bin/env python3
# Copyright (c) 2022 The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Generate and verify Quicksilver manual pages."""

import argparse
import difflib
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


BINARIES = [
    "bin/quicksilver-daemon",
    "bin/quicksilver-cli",
    "bin/quicksilver-tx",
    "bin/quicksilver-vault",
    "bin/quicksilver",
    "bin/quicksilver-agent",
]

VERSION_RE = re.compile(r"v\d+\.\d+\.\d+(?:rc\d+)?(?:-[0-9a-f]{12})?")


def run(command, **kwargs):
    return subprocess.run(command, check=True, text=True, **kwargs)


def project_release(cmake_file):
    """Return the release version encoded by the top-level CMake file."""
    text = Path(cmake_file).read_text(encoding="utf8")
    values = {}
    for name in ("MAJOR", "MINOR", "BUILD", "RC"):
        match = re.search(
            rf"^set\(CLIENT_VERSION_{name} ([0-9]+)\)$", text, re.MULTILINE
        )
        if match is None:
            raise ValueError(f"cannot read CLIENT_VERSION_{name} from {cmake_file}")
        values[name] = int(match.group(1))
    version = f"v{values['MAJOR']}.{values['MINOR']}.{values['BUILD']}"
    if values["RC"]:
        version += f"rc{values['RC']}"
    return version


def git_output(git, topdir, *args):
    return subprocess.run(
        [git, "-C", str(topdir), *args],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    ).stdout.rstrip()


def source_date_epoch(git, topdir):
    """Pin help2man's date to the commit that established the release version."""
    value = git_output(git, topdir, "log", "-1", "--format=%ct", "--", "CMakeLists.txt")
    if not value.isdigit():
        raise ValueError("cannot derive SOURCE_DATE_EPOCH from CMakeLists.txt history")
    return value


def read_binary_versions(builddir, skip_missing):
    versions = []
    for relpath in BINARIES:
        abspath = Path(builddir) / relpath
        try:
            result = run([str(abspath), "--version"], stdout=subprocess.PIPE)
        except OSError:
            if skip_missing:
                print(
                    f"{abspath} not found or not an executable. Skipping...",
                    file=sys.stderr,
                )
                continue
            raise RuntimeError(f"{abspath} not found or not an executable") from None

        lines = result.stdout.splitlines()
        if (
            not lines
            or not lines[0].split()
            or not lines[0].split()[-1].startswith("v")
        ):
            raise RuntimeError(f"{abspath} returned an invalid version response")
        if len(lines) < 2 or not lines[1].startswith("Copyright (C)"):
            raise RuntimeError(f"{abspath} returned an invalid copyright response")
        versions.append((abspath, lines[0].split()[-1], lines[1:]))

    if not versions:
        raise RuntimeError(
            f"No binaries found in {builddir}. Please ensure the binaries are present "
            "in that directory, or set another build path using BUILDDIR."
        )
    return versions


def release_binary_version(release_version, git, topdir):
    expected_release = project_release(Path(topdir) / "CMakeLists.txt")
    if release_version != expected_release:
        raise RuntimeError(
            f"release version {release_version!r} does not match CMakeLists.txt "
            f"version {expected_release!r}"
        )
    head = git_output(git, topdir, "rev-parse", "--verify", "HEAD")
    return f"{release_version}-{head[:12]}"


def rewrite_release_version(page, binary_version, release_version):
    """Replace the honest pre-tag build identity in help2man's output."""
    text = page.read_text(encoding="utf8")
    escaped_binary = binary_version.replace("-", r"\-")
    escaped_release = release_version.replace("-", r"\-")
    replacements = text.count(binary_version) + text.count(escaped_binary)
    if replacements == 0:
        raise RuntimeError(f"{page} does not contain binary version {binary_version}")
    text = text.replace(binary_version, release_version)
    text = text.replace(escaped_binary, escaped_release)
    page.write_text(text, encoding="utf8")


def generate_pages(versions, mandir, help2man, epoch, release_version=None):
    mandir = Path(mandir)
    mandir.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    environment["SOURCE_DATE_EPOCH"] = epoch

    with tempfile.NamedTemporaryFile("w", suffix=".h2m") as footer:
        footer.write("[COPYRIGHT]\n")
        footer.write("\n".join(versions[0][2]).strip())
        footer.write("\n[SEE ALSO]\n")
        footer.write(", ".join(Path(path).name + "(1)" for path in BINARIES))
        footer.write("\n")
        footer.flush()

        for abspath, binary_version, _ in versions:
            displayed_version = release_version or binary_version
            outname = mandir / f"{abspath.name}.1"
            print(f"Generating {outname}...")
            run(
                [
                    help2man,
                    "-N",
                    f"--version-string={displayed_version}",
                    f"--include={footer.name}",
                    "-o",
                    str(outname),
                    str(abspath),
                ],
                env=environment,
            )
            if release_version:
                rewrite_release_version(outname, binary_version, release_version)


def section_line(lines, heading):
    try:
        index = lines.index(heading)
    except ValueError:
        return None
    for line in lines[index + 1 :]:
        if line.startswith(".SH "):
            break
        if line and not line.startswith("."):
            return line
    return None


def version_on_line(line):
    if line is None:
        return None
    match = VERSION_RE.search(line.replace(r"\-", "-"))
    return match.group(0) if match else None


def page_versions(page):
    lines = Path(page).read_text(encoding="utf8").splitlines()
    title = next((line for line in lines if line.startswith(".TH ")), None)
    name = section_line(lines, ".SH NAME")
    description = section_line(lines, ".SH DESCRIPTION")
    return [version_on_line(title), version_on_line(name), version_on_line(description)]


def check_consistency(mandir):
    """Check the release-time invariants without running any binaries."""
    mandir = Path(mandir)
    expected_names = [Path(path).name for path in BINARIES]
    expected_see_also = ", ".join(f"{name}(1)" for name in expected_names)
    errors = []
    versions_by_page = {}

    actual_pages = {path.name for path in mandir.glob("*.1")}
    expected_pages = {f"{name}.1" for name in expected_names}
    for name in sorted(expected_pages - actual_pages):
        errors.append(f"{mandir / name}: missing generated page")
    for name in sorted(actual_pages - expected_pages):
        errors.append(f"{mandir / name}: unexpected generated page")

    for binary_name in expected_names:
        page = mandir / f"{binary_name}.1"
        if not page.is_file():
            continue
        versions = page_versions(page)
        labels = (".TH", "NAME", "DESCRIPTION")
        for label, version in zip(labels, versions):
            if version is None:
                errors.append(f"{page}: {label} has no version string")
        present = [version for version in versions if version is not None]
        if present and any(version != present[0] for version in present[1:]):
            errors.append(
                f"{page}: version fields disagree: "
                + ", ".join(
                    f"{label}={version}" for label, version in zip(labels, versions)
                )
            )
        if len(present) == len(versions) and len(set(present)) == 1:
            versions_by_page[page] = present[0]

        lines = page.read_text(encoding="utf8").splitlines()
        see_also = section_line(lines, '.SH "SEE ALSO"')
        if see_also != expected_see_also:
            errors.append(
                f"{page}: SEE ALSO differs\n"
                f"  expected: {expected_see_also}\n"
                f"  actual:   {see_also or '<missing>'}"
            )

    if versions_by_page:
        counts = {}
        for version in versions_by_page.values():
            counts[version] = counts.get(version, 0) + 1
        common_version = max(counts, key=lambda version: (counts[version], version))
        for page, version in versions_by_page.items():
            if version != common_version:
                errors.append(
                    f"{page}: version {version} differs from common version {common_version}"
                )

    if errors:
        print("Manual-page consistency check failed:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return False
    version = next(iter(versions_by_page.values()))
    print(
        f"Manual-page consistency check passed: {len(expected_pages)} pages at {version}"
    )
    return True


def verify_pages(versions, mandir, help2man, epoch):
    with tempfile.TemporaryDirectory(prefix="quicksilver-manpages-") as tempdir:
        generate_pages(versions, tempdir, help2man, epoch)
        different = False
        for relpath in BINARIES:
            name = f"{Path(relpath).name}.1"
            committed = Path(mandir) / name
            generated = Path(tempdir) / name
            if not committed.is_file() or not generated.is_file():
                print(f"missing page while verifying: {name}", file=sys.stderr)
                different = True
                continue
            old = committed.read_text(encoding="utf8").splitlines(keepends=True)
            new = generated.read_text(encoding="utf8").splitlines(keepends=True)
            diff = list(
                difflib.unified_diff(
                    old, new, fromfile=str(committed), tofile=str(generated)
                )
            )
            if diff:
                different = True
                sys.stderr.writelines(diff)
        if different:
            print(
                "Manual-page verification failed: generated pages differ",
                file=sys.stderr,
            )
            return False
    print("Manual-page verification passed: generated pages match doc/man exactly")
    return True


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "-s",
        "--skip-missing-binaries",
        action="store_true",
        help="skip generation for binaries that are not found in the build path",
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        "--release-version",
        metavar="vX.Y.Z",
        help="generate pre-tag pages carrying the exact future release version",
    )
    mode.add_argument(
        "--check",
        action="store_true",
        help="check versions and SEE ALSO sections in the committed pages",
    )
    mode.add_argument(
        "--verify",
        action="store_true",
        help="at an exact clean tag, regenerate in a temporary directory and diff",
    )
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    git = os.getenv("GIT", "git")
    help2man = os.getenv("HELP2MAN", "help2man")
    topdir = Path(
        os.getenv("TOPDIR")
        or git_output(git, Path.cwd(), "rev-parse", "--show-toplevel")
    )
    builddir = Path(os.getenv("BUILDDIR", topdir / "build"))
    mandir = Path(os.getenv("MANDIR", topdir / "doc/man"))

    try:
        if args.check:
            return 0 if check_consistency(mandir) else 1
        if args.release_version and args.skip_missing_binaries:
            raise RuntimeError("--release-version requires every binary in BINARIES")
        if args.verify and args.skip_missing_binaries:
            raise RuntimeError("--verify requires every binary in BINARIES")

        release_version = project_release(topdir / "CMakeLists.txt")
        if args.verify:
            dirty = git_output(
                git, topdir, "status", "--porcelain", "--untracked-files=no"
            )
            if dirty:
                raise RuntimeError("--verify requires a clean tracked checkout")
            tags = git_output(git, topdir, "tag", "--points-at", "HEAD").splitlines()
            if release_version not in tags:
                raise RuntimeError(
                    f"--verify requires HEAD to carry the exact tag {release_version}"
                )

        versions = read_binary_versions(builddir, args.skip_missing_binaries)
        if args.release_version:
            expected_binary = release_binary_version(args.release_version, git, topdir)
            mismatches = [
                f"{path}: expected {expected_binary}, got {version}"
                for path, version, _ in versions
                if version != expected_binary
            ]
            if mismatches:
                raise RuntimeError(
                    "release-mode binary identity check failed:\n"
                    + "\n".join(mismatches)
                )
        elif args.verify:
            mismatches = [
                f"{path}: expected {release_version}, got {version}"
                for path, version, _ in versions
                if version != release_version
            ]
            if mismatches:
                raise RuntimeError(
                    "tagged binary identity check failed:\n" + "\n".join(mismatches)
                )
        elif any(version.endswith("-dirty") for _, version, _ in versions):
            print("WARNING: Binaries were built from a dirty tree.")
            print("man pages generated from dirty binaries should NOT be committed.")
            print("Commit or discard changes, rebuild, then run this script again.\n")

        epoch = source_date_epoch(git, topdir)
        print("SOURCE_DATE_EPOCH=" f"{epoch} (last commit that changed CMakeLists.txt)")
        if args.verify:
            return 0 if verify_pages(versions, mandir, help2man, epoch) else 1
        generate_pages(versions, mandir, help2man, epoch, args.release_version)
        return 0
    except (RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
