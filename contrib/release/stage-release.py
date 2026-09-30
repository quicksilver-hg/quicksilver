#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""Stage one release tree for every platform named on the command line.

The script copies an explicit file list out of each platform's input
directory, checks Debian packages whose binaries it can execute, runs the
optional pre-hash signing hook, then hashes. It has no built-in platform
list: a new platform is another --platform argument and a provenance record.

Provenance, in the input directory, is a UTF-8 file named PROVENANCE.
Blank lines and lines starting with # are ignored. Every other line is
key=value, split on the first equals sign. These keys are required once:

    platform, public_tag, public_sha, development_sha, builder_host,
    os_userland, compiler, image_digest, build_command, version_line

image_digest is the image the builder used, or the word none when that
build had no image. file repeats, once per public basename. The script
copies those names and nothing else in the directory, so logs and scratch
stay out of the signed set.

--sign-hook PLATFORM=EXECUTABLE is the pre-hash slot. After the copy and
the transfer-hash check, and before gen-sha256sums.sh, the executable is
run with the staged platform directory as its only argument. It may
rewrite bytes in place. It may not add, remove, or rename files. Omit the
option and that platform is hashed as copied. The slot is not a Windows
branch; any platform may set it, and Authenticode is the reason it exists.
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


RELEASE_VERSION = re.compile(
    r"^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:rc([1-9][0-9]*))?$"
)
# A Debian revision is -N with N a positive integer and no leading zero.
DEBIAN_REVISION = re.compile(r"^[1-9][0-9]*$")
HEX40 = re.compile(r"^[0-9a-fA-F]{40}$")
PLATFORM_NAME = re.compile(r"^[a-z0-9][a-z0-9.+_-]{0,63}$")
FILE_NAME = re.compile(r"^[A-Za-z0-9._~+-]+$")
VERSION_TOKEN = re.compile(r"v\d+\.\d+\.\d+\S*")
PROVENANCE_KEYS = (
    "platform",
    "public_tag",
    "public_sha",
    "development_sha",
    "builder_host",
    "os_userland",
    "compiler",
    "image_digest",
    "build_command",
    "version_line",
)
RESERVED_NAMES = {
    "PROVENANCE",
    "SHA256SUMS",
    "SHA256SUMS.asc",
    "INDEX",
    "INDEX.asc",
}
INDEX_FIELDS = (
    "builder_host",
    "os_userland",
    "compiler",
    "image_digest",
    "build_command",
    "version_line",
)


class ReleaseError(Exception):
    pass


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def copy_verified(src, dst, copy=shutil.copyfile):
    """Copy src to dst and refuse if the destination bytes differ."""
    before = sha256_file(src)
    copy(src, dst)
    shutil.copymode(src, dst)
    after = sha256_file(dst)
    if before != after:
        raise ReleaseError(f"copy changed bytes of {src.name}: {before} != {after}")


def version_tokens(text):
    return VERSION_TOKEN.findall(text)


def require_exact_version(text, version, what):
    tokens = version_tokens(text)
    bad = [token for token in tokens if token != version]
    if bad:
        raise ReleaseError(f"{what} identifies as {bad[0]}, not {version}")
    if version not in tokens:
        raise ReleaseError(f"{what} does not identify as {version}")


def read_provenance(path):
    if not path.is_file():
        raise ReleaseError(f"provenance record is missing: {path}")
    fields = {}
    files = []
    for lineno, raw in enumerate(path.read_text(encoding="utf8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise ReleaseError(f"{path}:{lineno}: expected key=value")
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip()
        if not key or not value:
            raise ReleaseError(f"{path}:{lineno}: empty key or value")
        if any(char in value for char in "\n\r"):
            raise ReleaseError(f"{path}:{lineno}: value contains a newline")
        if key == "file":
            files.append(value)
            continue
        if key in fields:
            raise ReleaseError(f"duplicate provenance field {key} in {path}")
        fields[key] = value
    missing = [key for key in PROVENANCE_KEYS if key not in fields]
    if missing:
        raise ReleaseError(
            f"missing provenance field {missing[0]} in {path}"
        )
    if not files:
        raise ReleaseError(f"provenance record lists no files: {path}")
    return fields, files


def check_file_name(name):
    if name in RESERVED_NAMES or not FILE_NAME.fullmatch(name) or name in {".", ".."}:
        raise ReleaseError(f"not a plain file name: {name}")


def parse_pair(text, option):
    if "=" not in text:
        raise ReleaseError(f"{option} must be NAME=VALUE, not {text!r}")
    name, value = text.split("=", 1)
    if not name or not value:
        raise ReleaseError(f"{option} must be NAME=VALUE, not {text!r}")
    return name, value


def regular_names(directory):
    names = set()
    unexpected = []
    for entry in directory.iterdir():
        if entry.is_symlink() or not entry.is_file():
            unexpected.append(entry.name)
            continue
        names.add(entry.name)
    if unexpected:
        raise ReleaseError(
            "staged directory has a non-regular entry: " + ", ".join(sorted(unexpected))
        )
    return names


def run_hook(executable, platform_dir):
    before = regular_names(platform_dir)
    result = subprocess.run([str(executable), str(platform_dir)], check=False)
    if result.returncode != 0:
        raise ReleaseError(
            f"sign hook for {platform_dir.name} exited {result.returncode}"
        )
    try:
        after = regular_names(platform_dir)
    except ReleaseError as exc:
        raise ReleaseError(
            f"sign hook for {platform_dir.name} changed the file set: {exc}"
        ) from exc
    added = sorted(after - before)
    removed = sorted(before - after)
    if added or removed:
        detail = []
        if added:
            detail.append("added " + ", ".join(added))
        if removed:
            detail.append("removed " + ", ".join(removed))
        raise ReleaseError(
            f"sign hook for {platform_dir.name} changed the file set: " + "; ".join(detail)
        )


def debian_upstream(version):
    """Upstream version derived from a release tag, with no Debian revision."""
    match = RELEASE_VERSION.fullmatch(version)
    if match is None:
        raise ReleaseError(f"cannot derive Debian version from {version}")
    upstream = ".".join(match.groups()[:3])
    if match.group(4) is not None:
        upstream += f"~rc{match.group(4)}"
    return upstream


def identify_deb(deb, version, accepted_revision=None):
    """Refuse a .deb whose packaged executables are not exactly this release.

    The upstream version comes from the tag (`X.Y.Z` or `X.Y.Z~rcN`). The
    revision may be any `-N`, because a packaging-only rebuild of that tag
    is `-2`. Staging sees builder output and has no source tree, so it does
    not read debian/changelog. `accepted_revision` is the revision of a `.deb`
    already accepted in this run, or None for the first one. A later `.deb`
    with a different revision is refused, and the message names both.

    A Debian package is the artifact staging can open on Linux. Anything
    else, including a Windows installer, is not inspected: the provenance
    record and the transfer hash are the checks for that file.
    """
    upstream = debian_upstream(version)
    declared = subprocess.run(
        ["dpkg-deb", "--field", str(deb), "Version"],
        capture_output=True,
        text=True,
        encoding="utf8",
        errors="replace",
        check=False,
    )
    if declared.returncode != 0:
        detail = (declared.stderr or declared.stdout).strip()
        raise ReleaseError(f"could not read Debian version from {deb.name}: {detail}")
    declared_version = declared.stdout.strip()
    prefix = upstream + "-"
    if declared_version.startswith(prefix):
        revision = declared_version[len(prefix):]
    else:
        revision = None
    if revision is None or DEBIAN_REVISION.fullmatch(revision) is None:
        raise ReleaseError(
            f"{deb.name} has Debian version {declared_version}, not {upstream}-1"
        )
    if accepted_revision is not None and revision != accepted_revision:
        raise ReleaseError(
            f"{deb.name} has Debian revision -{revision}, not -{accepted_revision}"
        )
    with tempfile.TemporaryDirectory() as extracted:
        result = subprocess.run(
            ["dpkg-deb", "-x", str(deb), extracted],
            capture_output=True,
            text=True,
            encoding="utf8",
            errors="replace",
            check=False,
        )
        if result.returncode != 0:
            detail = (result.stderr or result.stdout).strip()
            raise ReleaseError(f"could not extract {deb.name}: {detail}")
        ran = False
        for path in sorted(Path(extracted).rglob("*")):
            if not path.is_file() or path.is_symlink():
                continue
            if path.stat().st_mode & 0o111 == 0:
                continue
            ran = True
            identify_binary(path, version)
        if not ran:
            raise ReleaseError(f"{deb.name} contains no executable to identify")
    return revision


def identify_binary(path, version):
    env = os.environ.copy()
    env["QT_QPA_PLATFORM"] = "offscreen"
    try:
        result = subprocess.run(
            [str(path), "-version"],
            capture_output=True,
            text=True,
            encoding="utf8",
            errors="replace",
            timeout=60,
            check=False,
            env=env,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise ReleaseError(f"{path.name} could not be executed: {exc}") from exc
    require_exact_version(result.stdout + "\n" + result.stderr, version, path.name)
    if result.returncode != 0:
        raise ReleaseError(f"{path.name} -version exited {result.returncode}")


def read_sums(path):
    entries = {}
    for line in path.read_text(encoding="utf8").splitlines():
        if "  " not in line:
            raise ReleaseError(f"bad checksum line in {path}: {line}")
        digest, name = line.split("  ", 1)
        entries[name] = digest
    return entries


def write_index(version_dir, version, public_sha, development_sha, platforms):
    lines = [
        f"version={version}",
        f"public_tag={version}",
        f"public_sha={public_sha}",
        f"development_sha={development_sha}",
    ]
    for platform in platforms:
        sums = read_sums(version_dir / platform["name"] / "SHA256SUMS")
        lines.append("")
        lines.append(f"platform={platform['name']}")
        lines.append(f"directory={platform['name']}")
        for key in INDEX_FIELDS:
            lines.append(f"{key}={platform['fields'][key]}")
        for name in sorted(platform["files"]):
            if name not in sums:
                raise ReleaseError(f"{platform['name']}/SHA256SUMS does not list {name}")
            size = (version_dir / platform["name"] / name).stat().st_size
            lines.append(f"file={name} size={size} sha256={sums[name]}")
        extra = sorted(set(sums) - set(platform["files"]))
        if extra:
            raise ReleaseError(
                f"{platform['name']}/SHA256SUMS lists {extra[0]}, which was not staged"
            )
    lines.append("")
    (version_dir / "INDEX").write_text("\n".join(lines), encoding="utf8")


def write_top_sums(version_dir, platforms):
    names = sorted([f"{platform['name']}/SHA256SUMS" for platform in platforms] + ["INDEX"])
    result = subprocess.run(
        ["sha256sum", "--", *names],
        cwd=version_dir,
        capture_output=True,
        text=True,
        encoding="utf8",
        check=False,
    )
    if result.returncode != 0:
        raise ReleaseError(f"sha256sum failed: {result.stderr.strip()}")
    (version_dir / "SHA256SUMS").write_text(result.stdout, encoding="utf8")


def load_platforms(pairs, version, public_sha, development_sha):
    platforms = []
    seen = set()
    for name, raw_input in pairs:
        if not PLATFORM_NAME.fullmatch(name):
            raise ReleaseError(f"not a platform name: {name}")
        if name in seen:
            raise ReleaseError(f"duplicate platform {name}")
        seen.add(name)
        source = Path(raw_input)
        if not source.is_dir():
            raise ReleaseError(f"platform input is not a directory: {source}")
        fields, files = read_provenance(source / "PROVENANCE")
        if fields["platform"] != name:
            raise ReleaseError(
                f"provenance platform is {fields['platform']}, not {name}"
            )
        if fields["public_tag"] != version:
            raise ReleaseError(
                f"provenance public_tag for {name} is {fields['public_tag']}, not {version}"
            )
        if fields["public_sha"].lower() != public_sha:
            raise ReleaseError(
                f"provenance public_sha for {name} does not match the release"
            )
        if fields["development_sha"].lower() != development_sha:
            raise ReleaseError(
                f"provenance development_sha for {name} does not match the release"
            )
        require_exact_version(fields["version_line"], version, f"provenance version_line for {name}")
        seen_files = set()
        for file_name in files:
            check_file_name(file_name)
            if file_name in seen_files:
                raise ReleaseError(f"duplicate file name {file_name} in {name}")
            seen_files.add(file_name)
            candidate = source / file_name
            if not candidate.is_file() or candidate.is_symlink():
                raise ReleaseError(f"named file does not exist: {name}/{file_name}")
        platforms.append({"name": name, "source": source, "fields": fields, "files": files})
    return platforms


def stage(version, public_sha, development_sha, output, platforms, hooks):
    version_dir = Path(output) / version
    if version_dir.exists():
        raise ReleaseError(f"{version_dir} already exists")
    version_dir.parent.mkdir(parents=True, exist_ok=True)
    version_dir.mkdir()
    try:
        for platform in platforms:
            destination = version_dir / platform["name"]
            destination.mkdir()
            for file_name in platform["files"]:
                copy_verified(
                    platform["source"] / file_name,
                    destination / file_name,
                )
        accepted_revision = None
        for platform in platforms:
            hook = hooks.get(platform["name"])
            if hook is not None:
                run_hook(hook, version_dir / platform["name"])
            for file_name in platform["files"]:
                staged = version_dir / platform["name"] / file_name
                if file_name.endswith(".deb"):
                    accepted_revision = identify_deb(
                        staged, version, accepted_revision
                    )
        generator = Path(__file__).resolve().parent / "gen-sha256sums.sh"
        for platform in platforms:
            result = subprocess.run(
                [str(generator), str(version_dir / platform["name"])],
                capture_output=True,
                text=True,
                encoding="utf8",
                check=False,
            )
            if result.returncode != 0:
                detail = (result.stderr or result.stdout).strip()
                raise ReleaseError(f"gen-sha256sums.sh failed for {platform['name']}: {detail}")
        write_index(version_dir, version, public_sha, development_sha, platforms)
        write_top_sums(version_dir, platforms)
    except Exception:
        shutil.rmtree(version_dir)
        raise
    return version_dir


def parse_args(argv):
    parser = argparse.ArgumentParser(description="Stage a Quicksilver release tree.")
    parser.add_argument("--version", required=True)
    parser.add_argument("--public-sha", required=True)
    parser.add_argument("--development-sha", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--platform", action="append", default=[])
    parser.add_argument("--sign-hook", action="append", default=[])
    return parser.parse_args(argv)


def main(argv):
    try:
        args = parse_args(argv)
        if not args.platform:
            raise ReleaseError("at least one --platform NAME=INPUT_DIR is required")
        if not RELEASE_VERSION.fullmatch(args.version):
            raise ReleaseError(f"version must be vX.Y.Z or vX.Y.ZrcN, not {args.version}")
        if not HEX40.fullmatch(args.public_sha):
            raise ReleaseError("public SHA must be 40 hex digits")
        if not HEX40.fullmatch(args.development_sha):
            raise ReleaseError("development SHA must be 40 hex digits")
        public_sha = args.public_sha.lower()
        development_sha = args.development_sha.lower()
        pairs = [parse_pair(item, "--platform") for item in args.platform]
        platforms = load_platforms(pairs, args.version, public_sha, development_sha)
        hooks = {}
        names = {platform["name"] for platform in platforms}
        for item in args.sign_hook:
            name, raw_path = parse_pair(item, "--sign-hook")
            if name not in names:
                raise ReleaseError(f"sign hook names an unknown platform {name}")
            if name in hooks:
                raise ReleaseError(f"duplicate sign hook for {name}")
            executable = Path(raw_path)
            if not executable.is_file() or not os.access(executable, os.X_OK):
                raise ReleaseError(f"sign hook is not an executable file: {executable}")
            hooks[name] = executable
        version_dir = stage(
            args.version,
            public_sha,
            development_sha,
            args.output,
            platforms,
            hooks,
        )
    except ReleaseError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    print(version_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
