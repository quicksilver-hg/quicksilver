#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""Self-test for release staging, signing, and verification.

The keys live in a temporary GNUPGHOME this module creates and deletes.
Nothing here contacts the network or uses a project key.
"""

import hashlib
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


HERE = Path(__file__).resolve().parent
STAGE = HERE / "stage-release.py"
SIGN = HERE / "sign-release.sh"
VERIFY = HERE / "verify-release.sh"
VERSION = "v1.2.3"
PUBLIC = "a" * 40
DEV = "b" * 40
PROVENANCE_ORDER = (
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


def load_stage():
    spec = importlib.util.spec_from_file_location("stage_release", STAGE)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {STAGE}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


STAGE_MODULE = load_stage()


def provenance_text(platform, files, omit=(), **overrides):
    fields = {
        "platform": platform,
        "public_tag": VERSION,
        "public_sha": PUBLIC,
        "development_sha": DEV,
        "builder_host": "builder.example",
        "os_userland": "test userland",
        "compiler": "test compiler",
        "image_digest": "none",
        "build_command": "test build",
        "version_line": f"Quicksilver version {VERSION}",
    }
    fields.update(overrides)
    lines = [f"{key}={fields[key]}" for key in PROVENANCE_ORDER if key not in omit]
    for name in files:
        lines.append(f"file={name}")
    return "\n".join(lines) + "\n"


def version_script(line):
    return f"#!/bin/sh\nprintf '%s\\n' '{line}'\n"


def make_deb(dest, files):
    """files is a list of (archive path, body, mode)."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp) / "pkg"
        control = root / "DEBIAN"
        control.mkdir(parents=True)
        control.chmod(0o755)
        (control / "control").write_text(
            "Package: quicksilver\n"
            "Version: 1.2.3-1\n"
            "Architecture: amd64\n"
            "Maintainer: Quicksilver Release Test <release-test@example.invalid>\n"
            "Description: synthetic release-tooling package\n",
            encoding="utf8",
        )
        for rel, body, mode in files:
            path = root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(body, encoding="utf8")
            path.chmod(mode)
        result = subprocess.run(
            ["dpkg-deb", "--build", "--root-owner-group", str(root), str(dest)],
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode != 0:
            raise RuntimeError(result.stderr or result.stdout)


def write_hook(path, body):
    path.write_text("#!/bin/sh\n" + body + "\n", encoding="utf8")
    path.chmod(0o755)


class ReleaseToolingTest(unittest.TestCase):
    gpg_home = None
    fingerprint = None
    other_fingerprint = None

    @classmethod
    def setUpClass(cls):
        cls.gpg_home = tempfile.mkdtemp(prefix="qs-release-gpg-")
        os.chmod(cls.gpg_home, 0o700)
        Path(cls.gpg_home, "gpg-agent.conf").write_text(
            "allow-loopback-pinentry\n",
            encoding="utf8",
        )
        env = cls.gpg_env()
        cls.fingerprint = cls.generate_key(env, "Quicksilver Release Test <release-test@example.invalid>")
        cls.other_fingerprint = cls.generate_key(env, "Other Key <other-test@example.invalid>")

    @classmethod
    def tearDownClass(cls):
        env = cls.gpg_env()
        subprocess.run(["gpgconf", "--kill", "gpg-agent"], env=env, check=False)
        shutil.rmtree(cls.gpg_home, ignore_errors=True)

    @classmethod
    def gpg_env(cls):
        env = os.environ.copy()
        env["GNUPGHOME"] = cls.gpg_home
        env["QS_GPG_PASSPHRASE"] = ""
        env["LC_ALL"] = "C"
        return env

    @classmethod
    def generate_key(cls, env, uid):
        result = subprocess.run(
            [
                "gpg",
                "--batch",
                "--pinentry-mode",
                "loopback",
                "--passphrase",
                "",
                "--quick-gen-key",
                uid,
                "default",
                "default",
                "never",
            ],
            capture_output=True,
            text=True,
            env=env,
            check=False,
        )
        if result.returncode != 0:
            raise RuntimeError(result.stderr or result.stdout)
        listed = subprocess.run(
            ["gpg", "--batch", "--with-colons", "--list-secret-keys"],
            capture_output=True,
            text=True,
            env=env,
            check=True,
        )
        pending = None
        for line in listed.stdout.splitlines():
            fields = line.split(":")
            if fields[0] == "sec":
                pending = None
            elif fields[0] == "fpr" and pending is None:
                pending = fields[9]
            elif fields[0] == "uid" and uid in fields[9]:
                return pending
            elif fields[0] == "ssb":
                pending = "sub"
        raise RuntimeError(f"no fingerprint for {uid}\n{listed.stdout}")

    def assert_refused(self, result, needle):
        self.assertNotEqual(
            result.returncode,
            0,
            f"expected failure mentioning {needle!r}\n{result.stderr}",
        )
        self.assertIn(needle, result.stderr)

    def run_stage(self, output, platforms, hooks=(), version=VERSION, public=PUBLIC, development=DEV):
        command = [
            sys.executable,
            str(STAGE),
            "--version",
            version,
            "--public-sha",
            public,
            "--development-sha",
            development,
            "--output",
            str(output),
        ]
        for name, directory in platforms:
            command.extend(["--platform", f"{name}={directory}"])
        for name, hook in hooks:
            command.extend(["--sign-hook", f"{name}={hook}"])
        return subprocess.run(command, capture_output=True, text=True, check=False)

    def sign(self, staged, fingerprint=None):
        return subprocess.run(
            [str(SIGN), str(staged), fingerprint if fingerprint is not None else self.fingerprint],
            capture_output=True,
            text=True,
            env=self.gpg_env(),
            check=False,
        )

    def verify(self, staged, fingerprint=None):
        return subprocess.run(
            [str(VERIFY), str(staged), fingerprint if fingerprint is not None else self.fingerprint],
            capture_output=True,
            text=True,
            env=self.gpg_env(),
            check=False,
        )

    def write_plain(self, root, name, filename="payload.bin", content=b"payload", **overrides):
        directory = Path(root) / name
        directory.mkdir(parents=True)
        (directory / filename).write_bytes(content)
        (directory / "build.log").write_text("not shipped\n", encoding="utf8")
        omit = overrides.pop("omit", ())
        platform_field = overrides.pop("platform", name)
        (directory / "PROVENANCE").write_text(
            provenance_text(platform_field, [filename], omit=omit, **overrides),
            encoding="utf8",
        )
        return directory

    def write_deb_platform(self, root, name, identity_line, extra_binary=None):
        directory = Path(root) / name
        directory.mkdir(parents=True)
        files = [
            ("usr/bin/quicksilver-daemon", version_script(identity_line), 0o755),
        ]
        if extra_binary is not None:
            files.append(("usr/bin/quicksilver-cli", version_script(extra_binary), 0o755))
        deb_name = "quicksilver_1.2.3-1_amd64.deb"
        make_deb(directory / deb_name, files)
        shipped = [deb_name]
        for extra in ("quicksilver_1.2.3-1_amd64.ddeb", "quicksilver_1.2.3-1_amd64.buildinfo", "quicksilver_1.2.3-1_amd64.changes"):
            (directory / extra).write_text(extra + "\n", encoding="utf8")
            shipped.append(extra)
        (directory / "build.log").write_text("not shipped\n", encoding="utf8")
        (directory / "PROVENANCE").write_text(provenance_text(name, shipped), encoding="utf8")
        return directory

    def stage_plain(self, root, names):
        output = Path(root) / "out"
        platforms = [(name, self.write_plain(Path(root) / "in", name)) for name in names]
        result = self.run_stage(output, platforms)
        self.assertEqual(result.returncode, 0, result.stderr)
        return output / VERSION

    def test_happy_path_end_to_end(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            inputs = root / "in"
            jammy = self.write_deb_platform(
                inputs,
                "ubuntu-22.04",
                f"Quicksilver daemon version {VERSION}",
                extra_binary=f"Quicksilver RPC client version {VERSION}",
            )
            noble = self.write_deb_platform(
                inputs,
                "ubuntu-24.04",
                f"Quicksilver daemon version {VERSION}",
            )
            windows = root / "in" / "windows-x64"
            windows.mkdir()
            (windows / "installer.exe").write_bytes(b"installer-bytes")
            (windows / "copy.log").write_text("not shipped\n", encoding="utf8")
            (windows / "PROVENANCE").write_text(
                provenance_text("windows-x64", ["installer.exe"]),
                encoding="utf8",
            )
            output = root / "out"
            result = self.run_stage(
                output,
                [
                    ("ubuntu-22.04", jammy),
                    ("ubuntu-24.04", noble),
                    ("windows-x64", windows),
                ],
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            staged = output / VERSION
            self.assertFalse((staged / "ubuntu-22.04" / "build.log").exists())
            self.assertFalse((staged / "windows-x64" / "copy.log").exists())
            self.assertEqual((staged / "windows-x64" / "installer.exe").read_bytes(), b"installer-bytes")
            self.assertEqual((windows / "installer.exe").read_bytes(), b"installer-bytes")
            self.assertTrue((staged / "INDEX").is_file())
            self.assertTrue((staged / "ubuntu-22.04" / "SHA256SUMS").is_file())
            self.assertTrue((staged / "ubuntu-24.04" / "SHA256SUMS").is_file())
            self.assertTrue((staged / "windows-x64" / "SHA256SUMS").is_file())
            signed = self.sign(staged)
            self.assertEqual(signed.returncode, 0, signed.stderr)
            checked = self.verify(staged, self.fingerprint.lower())
            self.assertEqual(checked.returncode, 0, checked.stderr)
            self.assertTrue((staged / "INDEX.asc").is_file())
            self.assertTrue((staged / "windows-x64" / "SHA256SUMS.asc").is_file())

    def test_hook_byte_change_is_hashed_after_the_hook(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            windows = self.write_plain(root / "in", "windows-x64", "installer.exe", b"installer-bytes")
            hook = root / "hook.sh"
            write_hook(hook, "printf X >> \"$1/installer.exe\"")
            output = root / "out"
            result = self.run_stage(output, [("windows-x64", windows)], hooks=[("windows-x64", hook)])
            self.assertEqual(result.returncode, 0, result.stderr)
            staged_file = output / VERSION / "windows-x64" / "installer.exe"
            self.assertEqual(staged_file.read_bytes(), b"installer-bytesX")
            digest = hashlib.sha256(b"installer-bytesX").hexdigest()
            original = hashlib.sha256(b"installer-bytes").hexdigest()
            sums = (output / VERSION / "windows-x64" / "SHA256SUMS").read_text(encoding="utf8")
            index = (output / VERSION / "INDEX").read_text(encoding="utf8")
            self.assertIn(digest, sums)
            self.assertNotIn(original, sums)
            self.assertIn(digest, index)
            self.assertEqual((windows / "installer.exe").read_bytes(), b"installer-bytes")

    def test_platform_name_is_not_a_script_case(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            directory = self.write_plain(root / "in", "example-arch")
            result = self.run_stage(root / "out", [("example-arch", directory)])
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((root / "out" / VERSION / "example-arch" / "payload.bin").is_file())

    def test_refuse_existing_staging_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            existing = root / "out" / VERSION
            existing.mkdir(parents=True)
            (existing / "keep").write_text("keep\n", encoding="utf8")
            directory = self.write_plain(root / "in", "example-arch")
            result = self.run_stage(root / "out", [("example-arch", directory)])
            self.assert_refused(result, "already exists")
            self.assertEqual((existing / "keep").read_text(encoding="utf8"), "keep\n")

    def test_refuse_missing_provenance(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            directory = root / "in"
            directory.mkdir()
            (directory / "payload.bin").write_bytes(b"payload")
            result = self.run_stage(root / "out", [("example-arch", directory)])
            self.assert_refused(result, "provenance record is missing")

    def test_refuse_provenance_public_sha_mismatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            directory = self.write_plain(root / "in", "example-arch", public_sha="c" * 40)
            result = self.run_stage(root / "out", [("example-arch", directory)])
            self.assert_refused(result, "public_sha")

    def test_refuse_provenance_development_sha_mismatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            directory = self.write_plain(root / "in", "example-arch", development_sha="d" * 40)
            result = self.run_stage(root / "out", [("example-arch", directory)])
            self.assert_refused(result, "development_sha")

    def test_refuse_provenance_tag_mismatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            first = self.write_plain(root / "in", "ubuntu-22.04")
            second = self.write_plain(root / "in", "ubuntu-24.04", public_tag="v9.9.9")
            result = self.run_stage(root / "out", [("ubuntu-22.04", first), ("ubuntu-24.04", second)])
            self.assert_refused(result, "public_tag")
            self.assertIn("v9.9.9", result.stderr)

    def test_refuse_provenance_platform_mismatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            directory = self.write_plain(root / "in", "example-arch", platform="other-arch")
            result = self.run_stage(root / "out", [("example-arch", directory)])
            self.assert_refused(result, "other-arch")

    def test_refuse_missing_provenance_field(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            directory = self.write_plain(root / "in", "example-arch", omit=("compiler",))
            result = self.run_stage(root / "out", [("example-arch", directory)])
            self.assert_refused(result, "missing provenance field compiler")

    def test_refuse_missing_named_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            directory = root / "example-arch"
            directory.mkdir()
            (directory / "PROVENANCE").write_text(
                provenance_text("example-arch", ["missing.bin"]),
                encoding="utf8",
            )
            result = self.run_stage(root / "out", [("example-arch", directory)])
            self.assert_refused(result, "missing.bin")

    def test_refuse_file_name_not_basename(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            directory = root / "example-arch"
            directory.mkdir()
            (directory / "PROVENANCE").write_text(
                provenance_text("example-arch", ["../payload.bin"]),
                encoding="utf8",
            )
            result = self.run_stage(root / "out", [("example-arch", directory)])
            self.assert_refused(result, "not a plain file name")

    def test_refuse_duplicate_file_name(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            directory = self.write_plain(root / "in", "example-arch")
            text = (directory / "PROVENANCE").read_text(encoding="utf8")
            (directory / "PROVENANCE").write_text(text + "file=payload.bin\n", encoding="utf8")
            result = self.run_stage(root / "out", [("example-arch", directory)])
            self.assert_refused(result, "duplicate file name")

    def test_refuse_untagged_deb(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            directory = self.write_deb_platform(
                root / "in",
                "ubuntu-22.04",
                f"Quicksilver daemon version {VERSION}-0123456789ab",
            )
            result = self.run_stage(root / "out", [("ubuntu-22.04", directory)])
            self.assert_refused(result, f"{VERSION}-0123456789ab")
            self.assertFalse((root / "out" / VERSION).exists())

    def test_refuse_deb_different_version(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            directory = self.write_deb_platform(
                root / "in",
                "ubuntu-22.04",
                "Quicksilver daemon version v9.9.9",
            )
            result = self.run_stage(root / "out", [("ubuntu-22.04", directory)])
            self.assert_refused(result, "v9.9.9")

    def test_refuse_deb_without_executable(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            directory = root / "ubuntu-22.04"
            directory.mkdir(parents=True)
            deb = directory / "quicksilver_1.2.3-1_amd64.deb"
            make_deb(deb, [("usr/share/quicksilver/readme", "not a binary\n", 0o644)])
            (directory / "PROVENANCE").write_text(
                provenance_text("ubuntu-22.04", [deb.name]),
                encoding="utf8",
            )
            result = self.run_stage(root / "out", [("ubuntu-22.04", directory)])
            self.assert_refused(result, "no executable")

    def test_refuse_hook_adds_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            directory = self.write_plain(root / "in", "windows-x64", "installer.exe", b"installer-bytes")
            hook = root / "hook.sh"
            write_hook(hook, "printf extra > \"$1/extra.bin\"")
            result = self.run_stage(root / "out", [("windows-x64", directory)], hooks=[("windows-x64", hook)])
            self.assert_refused(result, "added extra.bin")

    def test_refuse_hook_removes_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            directory = self.write_plain(root / "in", "windows-x64", "installer.exe", b"installer-bytes")
            hook = root / "hook.sh"
            write_hook(hook, "rm -f \"$1/installer.exe\"")
            result = self.run_stage(root / "out", [("windows-x64", directory)], hooks=[("windows-x64", hook)])
            self.assert_refused(result, "removed installer.exe")

    def test_refuse_hook_nonzero(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            directory = self.write_plain(root / "in", "windows-x64", "installer.exe", b"installer-bytes")
            hook = root / "hook.sh"
            write_hook(hook, "exit 4")
            result = self.run_stage(root / "out", [("windows-x64", directory)], hooks=[("windows-x64", hook)])
            self.assert_refused(result, "exited 4")

    def test_refuse_short_public_sha(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = self.run_stage(Path(tmp) / "out", [("example-arch", Path(tmp))], public="abcd")
            self.assert_refused(result, "40 hex")

    def test_refuse_bad_version(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = self.run_stage(Path(tmp) / "out", [("example-arch", Path(tmp))], version="v1.2.3-rc1")
            self.assert_refused(result, "vX.Y.Z")

    def test_refuse_no_platform(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = self.run_stage(Path(tmp) / "out", [])
            self.assert_refused(result, "--platform")

    def test_refuse_duplicate_platform(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            directory = self.write_plain(root / "in", "example-arch")
            result = self.run_stage(
                root / "out",
                [("example-arch", directory), ("example-arch", directory)],
            )
            self.assert_refused(result, "duplicate platform")

    def test_refuse_input_not_a_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            result = self.run_stage(root / "out", [("example-arch", root / "missing")])
            self.assert_refused(result, "not a directory")

    def test_refuse_unknown_sign_hook(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            directory = self.write_plain(root / "in", "example-arch")
            result = self.run_stage(
                root / "out",
                [("example-arch", directory)],
                hooks=[("windows-x64", "/bin/true")],
            )
            self.assert_refused(result, "unknown platform")

    def test_refuse_copy_that_changes_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "source"
            dest = root / "dest"
            source.write_bytes(b"one")

            def corrupt(src, dst):
                del src
                dst.write_bytes(b"two")

            with self.assertRaises(STAGE_MODULE.ReleaseError) as caught:
                STAGE_MODULE.copy_verified(source, dest, copy=corrupt)
            self.assertIn("changed bytes", str(caught.exception))

    def test_refuse_existing_signature(self):
        with tempfile.TemporaryDirectory() as tmp:
            staged = self.stage_plain(tmp, ["example-arch"])
            first = self.sign(staged)
            self.assertEqual(first.returncode, 0, first.stderr)
            second = self.sign(staged)
            self.assert_refused(second, "signature file already exists")

    def test_refuse_short_fingerprint_on_sign(self):
        with tempfile.TemporaryDirectory() as tmp:
            staged = self.stage_plain(tmp, ["example-arch"])
            result = self.sign(staged, self.fingerprint[:16])
            self.assert_refused(result, "40 hex")

    def test_refuse_fingerprint_that_is_not_the_key(self):
        with tempfile.TemporaryDirectory() as tmp:
            staged = self.stage_plain(tmp, ["example-arch"])
            result = self.sign(staged, "ab" * 20)
            self.assert_refused(result, "no secret key")

    def test_refuse_sign_missing_index(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = self.sign(Path(tmp))
            self.assert_refused(result, "missing INDEX")

    def test_refuse_tampered_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            staged = self.stage_plain(tmp, ["example-arch"])
            signed = self.sign(staged)
            self.assertEqual(signed.returncode, 0, signed.stderr)
            payload = staged / "example-arch" / "payload.bin"
            payload.write_bytes(b"tampered")
            result = self.verify(staged)
            self.assert_refused(result, "payload.bin")

    def test_refuse_missing_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            staged = self.stage_plain(tmp, ["example-arch"])
            self.assertEqual(self.sign(staged).returncode, 0)
            (staged / "example-arch" / "payload.bin").unlink()
            result = self.verify(staged)
            self.assert_refused(result, "payload.bin")

    def test_refuse_extra_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            staged = self.stage_plain(tmp, ["example-arch"])
            self.assertEqual(self.sign(staged).returncode, 0)
            (staged / "example-arch" / "extra.bin").write_bytes(b"extra")
            result = self.verify(staged)
            self.assert_refused(result, "extra file in example-arch: extra.bin")

    def test_refuse_missing_signature(self):
        with tempfile.TemporaryDirectory() as tmp:
            staged = self.stage_plain(tmp, ["example-arch"])
            self.assertEqual(self.sign(staged).returncode, 0)
            (staged / "example-arch" / "SHA256SUMS.asc").unlink()
            result = self.verify(staged)
            self.assert_refused(result, "missing signature example-arch/SHA256SUMS.asc")

    def test_refuse_signature_by_other_key(self):
        with tempfile.TemporaryDirectory() as tmp:
            staged = self.stage_plain(tmp, ["example-arch"])
            self.assertEqual(self.sign(staged).returncode, 0)
            signature = staged / "INDEX.asc"
            signature.unlink()
            result = subprocess.run(
                [
                    "gpg",
                    "--batch",
                    "--pinentry-mode",
                    "loopback",
                    "--passphrase",
                    "",
                    "--armor",
                    "--detach-sign",
                    "--local-user",
                    self.other_fingerprint,
                    "--output",
                    str(signature),
                    str(staged / "INDEX"),
                ],
                capture_output=True,
                text=True,
                env=self.gpg_env(),
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            checked = self.verify(staged, self.fingerprint)
            self.assert_refused(checked, self.other_fingerprint)
            self.assertIn("signed by", checked.stderr)

    def test_refuse_wrong_fingerprint_argument(self):
        with tempfile.TemporaryDirectory() as tmp:
            staged = self.stage_plain(tmp, ["example-arch"])
            self.assertEqual(self.sign(staged).returncode, 0)
            result = self.verify(staged, self.other_fingerprint)
            self.assert_refused(result, self.fingerprint)
            self.assertIn(self.other_fingerprint, result.stderr)

    def test_refuse_short_fingerprint_on_verify(self):
        with tempfile.TemporaryDirectory() as tmp:
            staged = self.stage_plain(tmp, ["example-arch"])
            result = self.verify(staged, self.fingerprint[:8])
            self.assert_refused(result, "40 hex")

    def test_refuse_corrupt_signature(self):
        with tempfile.TemporaryDirectory() as tmp:
            staged = self.stage_plain(tmp, ["example-arch"])
            self.assertEqual(self.sign(staged).returncode, 0)
            signature = staged / "INDEX.asc"
            lines = signature.read_text(encoding="utf8").splitlines()
            for index, line in enumerate(lines):
                if line and not line.startswith("-") and not line.startswith("Version"):
                    lines[index] = ("A" if line[0] != "A" else "B") + line[1:]
                    break
            signature.write_text("\n".join(lines) + "\n", encoding="utf8")
            result = self.verify(staged)
            self.assert_refused(result, "signature verification failed")

    def test_refuse_index_hash_mismatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / VERSION
            platform = root / "example-arch"
            platform.mkdir(parents=True)
            (platform / "payload.bin").write_bytes(b"payload")
            summed = subprocess.run(
                ["sha256sum", "--", "payload.bin"],
                cwd=platform,
                capture_output=True,
                text=True,
                check=True,
            )
            (platform / "SHA256SUMS").write_text(summed.stdout, encoding="utf8")
            real = summed.stdout.split()[0]
            bad = ("a" if real[0] != "a" else "b") + real[1:]
            index = "\n".join(
                [
                    f"version={VERSION}",
                    f"public_tag={VERSION}",
                    f"public_sha={PUBLIC}",
                    f"development_sha={DEV}",
                    "",
                    "platform=example-arch",
                    "directory=example-arch",
                    "builder_host=builder.example",
                    "os_userland=test userland",
                    "compiler=test compiler",
                    "image_digest=none",
                    "build_command=test build",
                    f"version_line=Quicksilver version {VERSION}",
                    f"file=payload.bin size=7 sha256={bad}",
                    "",
                ]
            )
            (root / "INDEX").write_text(index, encoding="utf8")
            top = subprocess.run(
                ["sha256sum", "--", "example-arch/SHA256SUMS", "INDEX"],
                cwd=root,
                capture_output=True,
                text=True,
                check=True,
            )
            (root / "SHA256SUMS").write_text(top.stdout, encoding="utf8")
            signed = self.sign(root)
            self.assertEqual(signed.returncode, 0, signed.stderr)
            result = self.verify(root)
            self.assert_refused(result, "index hash mismatch for example-arch/payload.bin")


if __name__ == "__main__":
    unittest.main()
