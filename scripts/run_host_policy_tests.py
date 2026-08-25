#!/usr/bin/env python3
"""Compile and run platform-neutral firmware policy tests with strict diagnostics."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def find_compiler(explicit: str | None) -> str:
    candidates = [explicit, os.getenv("CC"), "clang", "cc", "gcc"]
    for candidate in candidates:
        if candidate and shutil.which(candidate):
            return candidate
    raise RuntimeError("no host C compiler found (set CC or pass --cc)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build-host-tests")
    args = parser.parse_args()

    try:
        compiler = find_compiler(args.cc)
    except RuntimeError as exc:
        parser.error(str(exc))

    args.build_dir.mkdir(parents=True, exist_ok=True)
    if args.sanitize:
        if os.name == "nt":
            parser.error("--sanitize is supported by the Linux/macOS CI compiler path")

    test_programs = {
        "controller_policy_test": [
            ROOT / "main" / "controller_policy.c",
            ROOT / "main" / "dshot_protocol.c",
            ROOT / "tests" / "c" / "test_controller_policy.c",
        ],
        "command_router_test": [
            ROOT / "main" / "controller_policy.c",
            ROOT / "main" / "command_router.c",
            ROOT / "tests" / "c" / "test_command_router.c",
        ],
    }
    for name, sources in test_programs.items():
        executable = args.build_dir / (f"{name}.exe" if os.name == "nt" else name)
        command = [
            compiler,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wpedantic",
            "-I",
            str(ROOT / "main"),
            *(str(source) for source in sources),
            "-o",
            str(executable),
        ]
        if args.sanitize:
            command[1:1] = [
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
            ]
        print(f"Compiling {name} with {compiler}")
        subprocess.run(command, cwd=ROOT, check=True)
        subprocess.run([str(executable)], cwd=ROOT, check=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as exc:
        print(f"host policy test command failed with exit code {exc.returncode}", file=sys.stderr)
        raise SystemExit(exc.returncode)
