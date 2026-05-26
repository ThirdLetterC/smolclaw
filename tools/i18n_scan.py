#!/usr/bin/env python3.14
"""Small source scanner for bare user-facing C strings.

This is intentionally conservative for Phase 17. It scans selected C source
files for direct printf/puts/fputs calls with string literals and expects
user-facing output to go through the i18n catalog first.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


CALL_RE = re.compile(r"\b(?:printf|fprintf|puts|fputs)\s*\([^;\n]*\"")


def scan_file(path: Path) -> list[tuple[int, str]]:
    findings: list[tuple[int, str]] = []
    for lineno, line in enumerate(
        path.read_text(encoding="utf-8").splitlines(), start=1
    ):
        stripped = line.strip()
        if not stripped or stripped.startswith("//") or stripped.startswith("/*"):
            continue
        if "sc_log_" in stripped or "sc_i18n_" in stripped:
            continue
        if CALL_RE.search(stripped):
            findings.append((lineno, stripped))
    return findings


def main(argv: list[str]) -> int:
    failed = False
    for arg in argv[1:]:
        path = Path(arg)
        for lineno, line in scan_file(path):
            failed = True
            print(f"{path}:{lineno}: bare user-facing string: {line}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
