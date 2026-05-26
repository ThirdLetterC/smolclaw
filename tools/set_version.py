#!/usr/bin/env python3.14
"""Set the SmolClaw project version in CMake metadata."""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path


VERSION_RE = re.compile(r"^(0|[1-9][0-9]*)(?:\.(0|[1-9][0-9]*)){2,3}$")
PROJECT_VERSION_RE = re.compile(
    r"(?P<prefix>project\(\s*SmolClawC\b(?:(?!\)).)*?\bVERSION\s+)"
    r"(?P<version>[0-9]+(?:\.[0-9]+){2,3})",
    re.DOTALL,
)
ABI_RE_TEMPLATE = r"(?m)^(set\(SC_ABI_VERSION_{part}\s+)([0-9]+)(\))$"


@dataclass(frozen=True)
class Version:
    major: int
    minor: int
    patch: int
    tweak: int | None = None

    @classmethod
    def parse(cls, raw: str) -> "Version":
        if VERSION_RE.fullmatch(raw) is None:
            raise ValueError(
                "version must be numeric CMake form: MAJOR.MINOR.PATCH[.TWEAK]"
            )

        parts = [int(part) for part in raw.split(".")]
        for part in parts:
            if part > 4_294_967_295:
                raise ValueError("version components must fit in uint32_t")

        tweak = parts[3] if len(parts) == 4 else None
        return cls(parts[0], parts[1], parts[2], tweak)

    def __str__(self) -> str:
        if self.tweak is None:
            return f"{self.major}.{self.minor}.{self.patch}"
        return f"{self.major}.{self.minor}.{self.patch}.{self.tweak}"


def fail(message: str) -> int:
    print(message, file=sys.stderr)
    return 1


def replace_once(
    pattern: re.Pattern[str], text: str, replacement: str
) -> tuple[str, str]:
    matches = list(pattern.finditer(text))
    if len(matches) != 1:
        raise ValueError(
            f"expected exactly one match for {pattern.pattern!r}, found {len(matches)}"
        )

    old_value = (
        matches[0].group("version")
        if "version" in pattern.groupindex
        else matches[0].group(2)
    )
    return pattern.sub(replacement, text, count=1), old_value


def set_abi_component(text: str, name: str, value: int) -> tuple[str, str]:
    pattern = re.compile(ABI_RE_TEMPLATE.format(part=name))
    replacement = rf"\g<1>{value}\3"
    return replace_once(pattern, text, replacement)


def update_cmake(
    cmake_path: Path, version: Version, update_abi: bool, dry_run: bool
) -> int:
    text = cmake_path.read_text(encoding="utf-8")
    updated, old_project = replace_once(
        PROJECT_VERSION_RE, text, rf"\g<prefix>{version}"
    )

    abi_changes: list[tuple[str, str, int]] = []
    if update_abi:
        for name, value in (
            ("MAJOR", version.major),
            ("MINOR", version.minor),
            ("PATCH", version.patch),
        ):
            updated, old_value = set_abi_component(updated, name, value)
            abi_changes.append((name.lower(), old_value, value))

    if updated == text:
        print(f"{cmake_path}: version already {version}")
        return 0

    print(f"{cmake_path}: project version {old_project} -> {version}")
    for name, old_value, value in abi_changes:
        print(f"{cmake_path}: ABI {name} {old_value} -> {value}")

    if dry_run:
        print("dry run: no files changed")
        return 0

    cmake_path.write_text(updated, encoding="utf-8")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Set the SmolClaw project version.")
    parser.add_argument(
        "version", help="Numeric CMake version: MAJOR.MINOR.PATCH[.TWEAK]"
    )
    parser.add_argument(
        "--source-dir",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="SmolClaw source directory. Defaults to the repository root.",
    )
    parser.add_argument(
        "--no-abi",
        action="store_true",
        help="Do not update SC_ABI_VERSION_MAJOR/MINOR/PATCH.",
    )
    parser.add_argument(
        "--dry-run", action="store_true", help="Print changes without writing files."
    )
    args = parser.parse_args(argv[1:])

    try:
        version = Version.parse(args.version)
    except ValueError as exc:
        return fail(str(exc))

    source_dir = args.source_dir.resolve()
    cmake_path = source_dir / "CMakeLists.txt"
    if not cmake_path.is_file():
        return fail(f"CMakeLists.txt not found: {cmake_path}")

    try:
        return update_cmake(cmake_path, version, not args.no_abi, args.dry_run)
    except OSError as exc:
        return fail(f"failed to update {cmake_path}: {exc}")
    except ValueError as exc:
        return fail(str(exc))


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
