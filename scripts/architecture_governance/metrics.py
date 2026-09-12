"""Own stable source measurements used by architecture governance."""

from __future__ import annotations

import hashlib
from collections.abc import Iterable
from pathlib import Path

from .model import ArchitecturePolicy


def production_line_count(path: Path) -> int:
    """Count nonblank physical lines after removing comment-only text."""

    lines = normalized_source(path).splitlines()
    if path.suffix == ".py":
        return sum(1 for line in lines if line.strip() and not line.lstrip().startswith("#"))
    count = 0
    inside_block_comment = False
    for line in lines:
        code, inside_block_comment = _strip_c_family_comments(line, inside_block_comment)
        if code.strip():
            count += 1
    return count


def source_fingerprint(root: Path, paths: Iterable[str]) -> str:
    """Return a newline-stable fingerprint for exact repository paths."""

    digest = hashlib.sha256()
    for relative_path in sorted(paths):
        normalized_path = relative_path.replace("\\", "/")
        digest.update(normalized_path.encode("utf-8"))
        digest.update(b"\0")
        digest.update(normalized_source(root / normalized_path).encode("utf-8"))
        digest.update(b"\0")
    return f"sha256:{digest.hexdigest()}"


def governed_source_areas(root: Path, policy: ArchitecturePolicy) -> dict[Path, str]:
    """Return every governed source path and its authoritative area."""

    governed: dict[Path, str] = {}
    for area in policy.areas:
        if area.status == "reserved":
            continue
        candidates = {
            path
            for source_root in area.source_roots
            for path in (root / source_root).rglob("*")
            if path.is_file() and path.suffix in policy.source_extensions
        }
        candidates.update(
            path
            for source_file in area.source_files
            if (path := root / source_file).is_file()
            and path.suffix in policy.source_extensions
        )
        for path in sorted(candidates):
            relative = path.relative_to(root).as_posix()
            if relative in policy.excluded_paths:
                continue
            previous = governed.setdefault(path, area.identifier)
            if previous != area.identifier:
                raise ValueError(
                    f"{relative} belongs to both {previous} and {area.identifier}"
                )
    return governed


def normalized_source(path: Path) -> str:
    """Read UTF-8 source with canonical newlines for cross-platform identity."""

    return path.read_text(encoding="utf-8").replace("\r\n", "\n").replace("\r", "\n")


def _strip_c_family_comments(line: str, inside_block: bool) -> tuple[str, bool]:
    """Remove C-family comments while preserving quoted comment markers."""

    output: list[str] = []
    index = 0
    quote: str | None = None
    escaped = False
    while index < len(line):
        current = line[index]
        following = line[index + 1] if index + 1 < len(line) else ""
        if inside_block:
            if current == "*" and following == "/":
                inside_block = False
                index += 2
            else:
                index += 1
            continue
        if quote is not None:
            output.append(current)
            if escaped:
                escaped = False
            elif current == "\\":
                escaped = True
            elif current == quote:
                quote = None
            index += 1
            continue
        if current in {'"', "'"}:
            quote = current
            output.append(current)
            index += 1
        elif current == "/" and following == "/":
            break
        elif current == "/" and following == "*":
            inside_block = True
            index += 2
        else:
            output.append(current)
            index += 1
    return "".join(output), inside_block
