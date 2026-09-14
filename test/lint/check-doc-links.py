#!/usr/bin/env python3
#
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
#
# Check repository documentation links without making network requests.

import argparse
import html
import io
import posixpath
import re
import subprocess
import sys
import tempfile
import tokenize
import unicodedata
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from urllib.parse import unquote, urlsplit


SKIP_PREFIXES = (
    PurePosixPath("src/crc32c"),
    PurePosixPath("src/crypto/ctaes"),
    PurePosixPath("src/leveldb"),
    PurePosixPath("src/secp256k1"),
)

# Intentional links to generated or otherwise untracked paths belong here. Keep
# exemptions exact so one stale-link exception cannot mask another source.
ALLOWED_MISSING_LINKS: set[tuple[PurePosixPath, str]] = set()

REFERENCE_DEFINITION_RE = re.compile(r"^ {0,3}\[(?!\^)(?:\\.|[^\]\n])+\]:[ \t]*(.*)$")
AUTOLINK_RE = re.compile(r"<((?:https?://|mailto:)[^<> \t]+)>", re.IGNORECASE)
FENCE_RE = re.compile(r"^ {0,3}(`{3,}|~{3,})")
ATX_HEADING_RE = re.compile(r"^ {0,3}#{1,6}[ \t]+(.+?)\s*$")
SETEXT_HEADING_RE = re.compile(r"^ {0,3}(?:=+|-+)[ \t]*$")
HTML_ANCHOR_RE = re.compile(
    r"<(?:a|[A-Za-z][A-Za-z0-9-]*)\b[^>]*\b(?:id|name)=[\"']([^\"']+)[\"']",
    re.IGNORECASE,
)
SCHEME_RE = re.compile(r"^[A-Za-z][A-Za-z0-9+.-]*:")
C_COMMENT_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".h",
    ".hpp",
    ".cu",
    ".cuh",
    ".m",
    ".mm",
    ".rs",
}
PYTHON_SUFFIXES = {".py", ".pyi"}
HASH_COMMENT_SUFFIXES = {".sh", ".bash", ".cmake"}


@dataclass(frozen=True)
class Link:
    source: PurePosixPath
    line: int
    target: str


@dataclass(frozen=True)
class ScanStats:
    source_files: int
    links: int
    local_links: int
    external_links: int
    anchor_links: int


def repo_root() -> Path:
    return Path(
        subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"], text=True, encoding="utf8"
        ).strip()
    )


def is_skipped(path: PurePosixPath) -> bool:
    return any(
        path == prefix or path.is_relative_to(prefix) for prefix in SKIP_PREFIXES
    )


def tracked_paths() -> list[PurePosixPath]:
    raw = subprocess.check_output(["git", "ls-files", "-z"], text=True, encoding="utf8")
    return [PurePosixPath(name) for name in raw.split("\0") if name]


def link_source_paths() -> list[PurePosixPath]:
    result = subprocess.run(
        [
            "git",
            "grep",
            "-I",
            "-l",
            "-z",
            "-e",
            "](",
            "-e",
            "]:",
            "-e",
            "<http",
            "-e",
            "<mailto:",
            "--",
        ],
        check=False,
        stdout=subprocess.PIPE,
        text=True,
        encoding="utf8",
    )
    if result.returncode not in {0, 1}:
        raise subprocess.CalledProcessError(result.returncode, result.args)
    return [PurePosixPath(name) for name in result.stdout.split("\0") if name]


def read_text_file(root: Path, path: PurePosixPath) -> str | None:
    disk_path = root / path
    if disk_path.is_symlink():
        return None
    try:
        data = disk_path.read_bytes()
    except OSError:
        return None
    if b"\0" in data:
        return None
    try:
        return data.decode("utf8")
    except UnicodeDecodeError:
        return None


def mask_inline_code(line: str) -> str:
    """Blank inline-code spans while preserving character offsets."""
    masked = list(line)
    position = 0
    while position < len(line):
        if line[position] != "`":
            position += 1
            continue
        run_end = position
        while run_end < len(line) and line[run_end] == "`":
            run_end += 1
        marker = line[position:run_end]
        close = line.find(marker, run_end)
        if close == -1:
            position = run_end
            continue
        for index in range(position, close + len(marker)):
            masked[index] = " "
        position = close + len(marker)
    return "".join(masked)


def mask_c_noncomments(line: str, in_block: bool) -> tuple[str, bool]:
    """Keep C-family comments in place and blank code/string content."""
    kept = [" "] * len(line)
    position = 0
    quote = None
    while position < len(line):
        if in_block:
            end = line.find("*/", position)
            if end == -1:
                kept[position:] = line[position:]
                break
            kept[position : end + 2] = line[position : end + 2]
            position = end + 2
            in_block = False
            continue
        char = line[position]
        if quote is not None:
            if char == "\\":
                position += 2
                continue
            if char == quote:
                quote = None
            position += 1
            continue
        if char in {'"', "'"}:
            quote = char
            position += 1
            continue
        if line.startswith("//", position):
            kept[position:] = line[position:]
            break
        if line.startswith("/*", position):
            end = line.find("*/", position + 2)
            if end == -1:
                kept[position:] = line[position:]
                in_block = True
                break
            kept[position : end + 2] = line[position : end + 2]
            position = end + 2
            continue
        position += 1
    return "".join(kept), in_block


def mask_hash_noncomments(line: str) -> str:
    """Keep a shell/Python-style comment and blank executable content."""
    quote = None
    position = 0
    while position < len(line):
        char = line[position]
        if quote is not None:
            if char == "\\":
                position += 2
                continue
            if char == quote:
                quote = None
            position += 1
            continue
        if char in {'"', "'"}:
            quote = char
        elif char == "#":
            return " " * position + line[position:]
        position += 1
    return " " * len(line)


def python_comment_lines(text: str) -> list[str]:
    """Return Python source with only tokenize-recognized comments retained."""
    original_lines = text.splitlines()
    kept = [[" "] * len(line) for line in original_lines]
    try:
        tokens = tokenize.generate_tokens(io.StringIO(text).readline)
        for token in tokens:
            if token.type != tokenize.COMMENT:
                continue
            row, column = token.start
            kept[row - 1][column : column + len(token.string)] = token.string
    except (IndentationError, tokenize.TokenError):
        return [" " * len(line) for line in original_lines]
    return ["".join(line) for line in kept]


def parse_destination(line: str, start: int) -> tuple[str, int] | None:
    """Parse one inline-link destination after the opening parenthesis."""
    while start < len(line) and line[start] in " \t":
        start += 1
    if start == len(line):
        return None
    if line[start] == "<":
        end = line.find(">", start + 1)
        if end == -1:
            return None
        return line[start + 1 : end], end + 1

    position = start
    depth = 0
    while position < len(line):
        char = line[position]
        if char == "\\" and position + 1 < len(line):
            position += 2
            continue
        if char == "(":
            depth += 1
        elif char == ")":
            if depth == 0:
                break
            depth -= 1
        elif char in " \t" and depth == 0:
            break
        position += 1
    if position == start:
        return None
    return line[start:position], position


def extract_links(source: PurePosixPath, text: str) -> list[Link]:
    links = []
    fence_marker = None
    in_block_comment = False
    is_markdown = source.suffix.lower() == ".md"
    python_comments = (
        python_comment_lines(text) if source.suffix.lower() in PYTHON_SUFFIXES else None
    )

    for line_number, original_line in enumerate(text.splitlines(), start=1):
        if is_markdown:
            fence_match = FENCE_RE.match(original_line)
            if fence_match:
                marker = fence_match.group(1)
                if fence_marker is None:
                    fence_marker = marker
                elif marker[0] == fence_marker[0] and len(marker) >= len(fence_marker):
                    fence_marker = None
                continue
            if fence_marker is not None:
                continue

        if is_markdown:
            line = mask_inline_code(original_line)
        elif python_comments is not None:
            line = python_comments[line_number - 1]
        elif source.suffix.lower() in C_COMMENT_SUFFIXES:
            line, in_block_comment = mask_c_noncomments(original_line, in_block_comment)
        elif (
            source.suffix.lower() in HASH_COMMENT_SUFFIXES
            or source.name == "CMakeLists.txt"
        ):
            line = mask_hash_noncomments(original_line)
        else:
            line = original_line
        occupied = []
        position = 0
        while (close := line.find("](", position)) != -1:
            opener = line.rfind("[", position, close)
            position = close + 2
            if (
                opener == -1
                or close == opener + 1
                or (opener > 0 and line[opener - 1] == "\\")
            ):
                continue
            parsed = parse_destination(line, position)
            if parsed is None:
                continue
            target, end = parsed
            links.append(Link(source, line_number, target))
            occupied.append((opener, end))

        definition = (
            REFERENCE_DEFINITION_RE.match(line)
            if is_markdown and "]:" in line
            else None
        )
        if definition:
            destination_start = definition.start(1)
            parsed = parse_destination(line, destination_start)
            if parsed is not None:
                target, end = parsed
                links.append(Link(source, line_number, target))
                occupied.append((definition.start(), end))

        autolinks = AUTOLINK_RE.finditer(line) if "<" in line else ()
        for match in autolinks:
            if any(start <= match.start() < end for start, end in occupied):
                continue
            links.append(Link(source, line_number, match.group(1)))

    return links


def clean_target(target: str) -> str:
    target = html.unescape(target.strip())
    return re.sub(r"\\([\\() <>#])", r"\1", target)


def external_error(target: str) -> str | None:
    if any(ord(char) < 32 or char.isspace() for char in target):
        return "external URL contains whitespace or a control character"
    parsed = urlsplit(target)
    scheme = parsed.scheme.lower()
    if scheme in {"http", "https"}:
        if not parsed.netloc:
            return "external URL has no host"
        return None
    if scheme == "mailto":
        if not parsed.path or "@" not in parsed.path:
            return "mailto URL has no address"
        return None
    return f"unsupported URL scheme {parsed.scheme!r}"


def local_target(source: PurePosixPath, path_text: str) -> PurePosixPath | None:
    decoded = unquote(path_text)
    candidate = PurePosixPath(posixpath.normpath(str(source.parent / decoded)))
    if candidate == PurePosixPath("."):
        return candidate
    if (
        candidate.is_absolute()
        or candidate == PurePosixPath("..")
        or candidate.is_relative_to("..")
    ):
        return None
    return candidate


def heading_slug(text: str) -> str:
    text = re.sub(r"[ \t]+#+[ \t]*$", "", text.strip())
    text = re.sub(r"!?\[([^]]+)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"<[^>]+>", "", text)
    text = text.replace("`", "").replace("*", "").replace("~", "")
    text = "".join(
        char
        for char in text.lower()
        if char in "-_ "
        or char.isspace()
        or not unicodedata.category(char).startswith(("P", "S", "C"))
    )
    return re.sub(r"\s+", "-", text)


def markdown_anchors(text: str) -> set[str]:
    anchors = set()
    slug_counts: dict[str, int] = {}
    fence_marker = None
    previous_line = None

    for line in text.splitlines():
        fence_match = FENCE_RE.match(line)
        if fence_match:
            marker = fence_match.group(1)
            if fence_marker is None:
                fence_marker = marker
            elif marker[0] == fence_marker[0] and len(marker) >= len(fence_marker):
                fence_marker = None
            previous_line = None
            continue
        if fence_marker is not None:
            continue

        heading = ATX_HEADING_RE.match(line)
        heading_text = heading.group(1) if heading else None
        if heading_text is None and previous_line and SETEXT_HEADING_RE.match(line):
            heading_text = previous_line.strip()
        if heading_text is not None:
            base = heading_slug(heading_text)
            duplicate = slug_counts.get(base, 0)
            slug_counts[base] = duplicate + 1
            anchors.add(base if duplicate == 0 else f"{base}-{duplicate}")
        anchors.update(html.unescape(anchor) for anchor in HTML_ANCHOR_RE.findall(line))
        previous_line = line if line.strip() else None
    return anchors


def target_exists(
    candidate: PurePosixPath,
    files: set[PurePosixPath],
    directories: set[PurePosixPath],
) -> bool:
    return candidate in files or candidate in directories


def markdown_target(
    candidate: PurePosixPath,
    files: set[PurePosixPath],
    directories: set[PurePosixPath],
) -> PurePosixPath | None:
    if candidate in files and candidate.suffix.lower() == ".md":
        return candidate
    if candidate in directories:
        readme = candidate / "README.md"
        if readme in files:
            return readme
    return None


def lint_links(
    root: Path,
    paths: list[PurePosixPath],
    sources: list[PurePosixPath] | None = None,
) -> tuple[list[str], set[tuple[PurePosixPath, PurePosixPath]], ScanStats]:
    files = set(paths)
    directories = {PurePosixPath(".")}
    for path in paths:
        directories.update(path.parents)

    texts = {}
    links = []
    failures = []
    for source in link_source_paths() if sources is None else sources:
        if is_skipped(source):
            continue
        text = read_text_file(root, source)
        if text is None:
            continue
        texts[source] = text
        links.extend(extract_links(source, text))

    anchor_cache = {}
    inbound = set()
    local_links = 0
    external_links = 0
    anchor_links = 0
    for link in links:
        target = clean_target(link.target)
        location = f"{link.source}:{link.line}"
        if not target:
            continue
        if target.startswith("//"):
            external_links += 1
            failures.append(
                f"{location}: external URL must include its scheme: {target}"
            )
            continue
        if target.startswith("/"):
            local_links += 1
            failures.append(f"{location}: root-relative link is forbidden: {target}")
            continue
        if SCHEME_RE.match(target):
            external_links += 1
            error = external_error(target)
            if error:
                failures.append(f"{location}: {error}: {target}")
            continue

        local_links += 1
        path_text, separator, fragment = target.partition("#")
        if separator:
            anchor_links += 1
        path_text = path_text.partition("?")[0]
        candidate = (
            link.source if not path_text else local_target(link.source, path_text)
        )
        if candidate is None:
            failures.append(f"{location}: link escapes the repository: {target}")
            continue

        allowed_missing = (link.source, target) in ALLOWED_MISSING_LINKS
        if not target_exists(candidate, files, directories):
            if not allowed_missing:
                failures.append(f"{location}: missing local target: {target}")
            continue

        target_doc = markdown_target(candidate, files, directories)
        if target_doc is not None and target_doc != link.source:
            inbound.add((link.source, target_doc))
        if not separator:
            continue
        if not fragment:
            failures.append(f"{location}: empty anchor: {target}")
            continue
        if target_doc is None:
            failures.append(f"{location}: anchor target is not Markdown: {target}")
            continue
        if target_doc not in anchor_cache:
            target_text = texts.get(target_doc)
            if target_text is None:
                target_text = read_text_file(root, target_doc)
            anchor_cache[target_doc] = markdown_anchors(target_text or "")
        decoded_fragment = unquote(fragment)
        if decoded_fragment not in anchor_cache[target_doc]:
            failures.append(
                f"{location}: missing anchor {decoded_fragment!r} in {target_doc}"
            )

    return (
        failures,
        inbound,
        ScanStats(
            source_files=len(texts),
            links=len(links),
            local_links=local_links,
            external_links=external_links,
            anchor_links=anchor_links,
        ),
    )


def orphan_failures(
    paths: list[PurePosixPath],
    inbound: set[tuple[PurePosixPath, PurePosixPath]],
) -> list[str]:
    linked_docs = {target for _source, target in inbound}
    return [
        f"{path}: orphan documentation file has no inbound local link"
        for path in paths
        if path.suffix.lower() == ".md"
        and path.is_relative_to("doc")
        and not is_skipped(path)
        and path not in linked_docs
    ]


def self_test() -> None:
    source = PurePosixPath("doc/source.md")
    text = """# One / Two!
## Repeated
## Repeated
[`kept`](target.md#heading) and `[`ignored`](missing.md)`
[reference]: ../README.md#quicksilver
<https://example.com/path>
```md
[ignored](missing.md)
# Not a heading
```
"""
    found = {(link.line, link.target) for link in extract_links(source, text)}
    assert found == {
        (4, "target.md#heading"),
        (5, "../README.md#quicksilver"),
        (6, "https://example.com/path"),
    }
    assert markdown_anchors(text) == {"one-two", "repeated", "repeated-1"}
    assert external_error("https://example.com/path") is None
    assert external_error("https:///missing-host") is not None
    assert local_target(source, "../../outside") is None

    with tempfile.TemporaryDirectory() as temporary_directory:
        root = Path(temporary_directory)
        fixtures = {
            PurePosixPath("README.md"): """# Fixture
[source](doc/source.md)
[root](/doc/target.md)
[missing](doc/missing.md)
[bad URL](https:///missing-host)
""",
            PurePosixPath("doc/source.md"): """# Source
[target](target.md#repeated-1)
[bad anchor](target.md#absent)
""",
            PurePosixPath("doc/target.md"): """# Target
## Repeated
## Repeated
""",
            PurePosixPath("doc/orphan.md"): "# Orphan\n",
        }
        for path, contents in fixtures.items():
            disk_path = root / path
            disk_path.parent.mkdir(parents=True, exist_ok=True)
            disk_path.write_text(contents, encoding="utf8")
        paths = list(fixtures)
        failures, inbound, stats = lint_links(root, paths, paths)
        assert failures == [
            "README.md:3: root-relative link is forbidden: /doc/target.md",
            "README.md:4: missing local target: doc/missing.md",
            "README.md:5: external URL has no host: https:///missing-host",
            "doc/source.md:3: missing anchor 'absent' in doc/target.md",
        ]
        assert orphan_failures(paths, inbound) == [
            "doc/orphan.md: orphan documentation file has no inbound local link"
        ]
        assert stats == ScanStats(
            source_files=4,
            links=6,
            local_links=5,
            external_links=1,
            anchor_links=2,
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run parser self-tests without scanning the repository",
    )
    args = parser.parse_args()

    self_test()
    if args.self_test:
        print("Documentation link self-test OK")
        return 0

    root = repo_root()
    paths = tracked_paths()
    failures, inbound, stats = lint_links(root, paths)
    failures.extend(orphan_failures(paths, inbound))

    if failures:
        print("Documentation link errors:")
        print("\n".join(failures))
        return 1
    documentation_files = sum(
        path.suffix.lower() == ".md"
        and path.is_relative_to("doc")
        and not is_skipped(path)
        for path in paths
    )
    print(
        f"Documentation links OK: checked {stats.links} links in "
        f"{stats.source_files} source files "
        f"({stats.local_links} local, {stats.external_links} external, "
        f"{stats.anchor_links} with anchors); "
        f"verified inbound links for {documentation_files} doc files"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
