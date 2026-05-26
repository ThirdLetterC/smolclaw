#!/usr/bin/env python3.14
"""Run clang-tidy against first-party C sources."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


def fail(message: str) -> int:
    print(message, file=sys.stderr)
    return 1


def run(command: list[str]) -> int:
    try:
        subprocess.run(command, check=True)
    except FileNotFoundError:
        return fail(f"required command not found: {command[0]}")
    except subprocess.CalledProcessError as exc:
        return exc.returncode
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Run clang-tidy against first-party C sources."
    )
    parser.add_argument(
        "--required", action="store_true", help="Fail when clang-tidy is unavailable."
    )
    args = parser.parse_args(argv[1:])

    if shutil.which("clang-tidy") is None:
        if args.required:
            return fail("clang-tidy is required but not installed")
        print(
            "clang-tidy not installed; skipping optional local check", file=sys.stderr
        )
        return 0

    croot = Path(__file__).resolve().parents[1]
    build_dir = Path(
        os.environ.get("BUILD_DIR", str(croot / "build" / "clang-tidy"))
    ).resolve()

    cmake_args = [
        "-DCMAKE_BUILD_TYPE=Debug",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
    ]
    if os.environ.get("SC_CLANG_TIDY_PROJECT_DEPS", "0") != "1":
        cmake_args.extend(
            [
                "-DSC_DEPS_PROVIDER=system",
                "-DSC_ENABLE_THIRD_PARTY_DEPS=OFF",
            ]
        )

    rc = run(["cmake", "-S", str(croot), "-B", str(build_dir), *cmake_args])
    if rc != 0:
        return rc

    sources = sorted(str(path) for path in (croot / "src").rglob("*.c"))
    if not sources:
        return fail("no C sources found for clang-tidy")

    third_party_root = croot / "vendor"
    tidy_args = [
        "--quiet",
        "-p",
        str(build_dir),
        "--extra-arg=-include",
        f"--extra-arg={croot / 'include' / 'compat' / 'c23_keyword_compat.h'}",
        "--extra-arg=-w",
    ]
    third_party_includes = [
        third_party_root / "clags" / "include",
        third_party_root / "jsonrpc" / "include",
        third_party_root / "microjson",
        third_party_root / "nanocron" / "include",
        third_party_root / "parson" / "include",
        third_party_root / "toml" / "include",
        third_party_root / "ulog" / "include",
        third_party_root / "websocket-client" / "include",
    ]
    for include_dir in third_party_includes:
        if include_dir.is_dir():
            tidy_args.append(f"--extra-arg=-I{include_dir}")

    return run(["clang-tidy", *tidy_args, *sources])


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
