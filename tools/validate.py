#!/usr/bin/env python3.14
"""Run SmolClaw validation modes."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


def fail(message: str, code: int = 1) -> int:
    print(message, file=sys.stderr)
    return code


def run(command: list[str]) -> int:
    try:
        subprocess.run(command, check=True)
    except FileNotFoundError:
        return fail(f"required command not found: {command[0]}")
    except subprocess.CalledProcessError as exc:
        return exc.returncode
    return 0


def run_ruff(tools_dir: Path) -> int:
    uv = shutil.which("uv")
    if uv is not None:
        return run(
            [
                uv,
                "run",
                "--project",
                str(tools_dir),
                "--locked",
                "ruff",
                "check",
                str(tools_dir),
            ]
        )

    ruff = shutil.which("ruff")
    if ruff is not None:
        return run([ruff, "check", str(tools_dir)])

    return fail("required command not found: uv or ruff")


def cmake_cache_value(build_dir: Path, key: str) -> str | None:
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        return None

    prefix = f"{key}:"
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(prefix):
            _, _, value = line.partition("=")
            return value
    return None


def cmake_cache_matches_source(build_dir: Path, source_dir: Path) -> bool:
    return cmake_cache_value(build_dir, "CMAKE_HOME_DIRECTORY") == str(source_dir)


def ensure_cmake_cache_source(build_dir: Path, source_dir: Path) -> None:
    cache = build_dir / "CMakeCache.txt"
    if cache.is_file() and not cmake_cache_matches_source(build_dir, source_dir):
        print(
            f"CMake cache in {build_dir} was generated for a different source tree; reconfiguring.",
            file=sys.stderr,
        )
        cache.unlink(missing_ok=True)
        shutil.rmtree(build_dir / "CMakeFiles", ignore_errors=True)
        return

    third_party_root = cmake_cache_value(build_dir, "SC_THIRD_PARTY_ROOT")
    if third_party_root == str(source_dir / "3rd-party"):
        print(
            f"CMake cache in {build_dir} uses the old vendored dependency root; reconfiguring.",
            file=sys.stderr,
        )
        cache.unlink(missing_ok=True)
        shutil.rmtree(build_dir / "CMakeFiles", ignore_errors=True)


def configure_build_test(
    source_dir: Path, build_dir: Path, cmake_args: list[str]
) -> int:
    ensure_cmake_cache_source(build_dir, source_dir)
    rc = run(
        [
            "cmake",
            "-S",
            str(source_dir),
            "-B",
            str(build_dir),
            "-DCMAKE_BUILD_TYPE=Debug",
            *cmake_args,
        ]
    )
    if rc != 0:
        return rc

    rc = run(["cmake", "--build", str(build_dir), "--parallel"])
    if rc != 0:
        return rc

    return run(["ctest", "--test-dir", str(build_dir), "--output-on-failure"])


def run_static(tools_dir: Path, source_dir: Path) -> int:
    if os.environ.get("SC_STATIC_SKIP_RUFF", "0") != "1":
        rc = run_ruff(tools_dir)
        if rc != 0:
            return rc

    rc = run(
        [
            "cppcheck",
            "--std=c23",
            "-Dnullptr=0",
            "--enable=warning,style,performance,portability",
            "--error-exitcode=1",
            "--inline-suppr",
            str(source_dir / "src"),
            str(source_dir / "tests"),
        ]
    )
    if rc != 0:
        return rc

    clang_tidy = tools_dir / "run_clang_tidy.py"
    if os.environ.get("SC_CLANG_TIDY_REQUIRED", "0") == "1":
        return run([sys.executable, str(clang_tidy), "--required"])
    return run([sys.executable, str(clang_tidy)])



def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Run SmolClaw validation modes.")
    parser.add_argument(
        "mode",
        nargs="?",
        default="fast",
        choices=("fast", "static", "sanitizer", "docs", "all"),
    )
    args = parser.parse_args(argv[1:])

    tools_dir = Path(__file__).resolve().parent
    source_dir = tools_dir.parent
    build_dir = Path(
        os.environ.get("BUILD_DIR", str(source_dir / "build" / "validate"))
    ).resolve()

    if args.mode == "fast":
        return configure_build_test(source_dir, build_dir, [])
    if args.mode == "sanitizer":
        return configure_build_test(
            source_dir, Path(f"{build_dir}-sanitizer"), ["-DSC_SANITIZERS=ON"]
        )
    if args.mode == "static":
        return run_static(tools_dir, source_dir)
    if args.mode == "all":
        steps = (
            lambda: configure_build_test(source_dir, build_dir, []),
            lambda: run_static(tools_dir, source_dir),
            lambda: configure_build_test(
                source_dir, Path(f"{build_dir}-sanitizer"), ["-DSC_SANITIZERS=ON"]
            ),
        )
        for step in steps:
            rc = step()
            if rc != 0:
                return rc
        return 0

    return fail("usage: validate.py [fast|static|sanitizer|docs|all]", 2)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
