#!/usr/bin/env python3
"""Compile and run firmware policy and lifecycle tests with strict diagnostics."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def find_compiler(explicit: str | None) -> str:
    candidates = [explicit, os.getenv("CC"), "clang", "cc", "gcc", "cl"]
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
        "scheduler_test": [ROOT / "tests" / "c" / "test_scheduler.c"],
        "tach_monitor_test": [ROOT / "tests" / "c" / "test_tach_monitor.c"],
        "fan_state_store_test": [ROOT / "tests" / "c" / "test_fan_state_store.c"],
        "state_publisher_test": [ROOT / "tests" / "c" / "test_state_publisher.c"],
        "mqtt_manager_test": [
            ROOT / "main" / "command_executor.c",
            ROOT / "main" / "command_router.c",
            ROOT / "main" / "controller_policy.c",
            ROOT / "tests" / "c" / "test_mqtt_manager.c",
        ],
        "ota_download_test": [
            ROOT / "main" / "controller_policy.c",
            ROOT / "main" / "ota_download.c",
            ROOT / "tests" / "c" / "test_ota_download.c",
        ],
        "ota_transport_test": [
            ROOT / "main" / "ota_transport.c",
            ROOT / "tests" / "c" / "test_ota_transport.c",
        ],
    }
    stub_directories = {
        "scheduler_test": ROOT / "tests" / "c" / "scheduler_stubs",
        "tach_monitor_test": ROOT / "tests" / "c" / "lifecycle_stubs",
        "fan_state_store_test": ROOT / "tests" / "c" / "lifecycle_stubs",
        "state_publisher_test": ROOT / "tests" / "c" / "lifecycle_stubs",
        "mqtt_manager_test": ROOT / "tests" / "c" / "lifecycle_stubs",
        "ota_download_test": ROOT / "tests" / "c" / "ota_stubs",
        "ota_transport_test": ROOT / "tests" / "c" / "ota_transport_stubs",
    }
    is_msvc = Path(compiler).name.lower() in ("cl", "cl.exe")
    for name, sources in test_programs.items():
        executable = args.build_dir / (f"{name}.exe" if os.name == "nt" else name)
        include_dirs = [ROOT / "main"]
        if name in stub_directories:
            include_dirs.insert(0, stub_directories[name])
        if is_msvc:
            command = [
                compiler, "/nologo", "/std:c11", "/W4", "/WX",
                *(f"/I{directory}" for directory in include_dirs),
                *(str(source) for source in sources),
                f"/Fe:{executable}",
                f"/Fo:{args.build_dir.resolve()}{os.sep}",
            ]
        else:
            command = [
                compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wpedantic",
                *(flag for directory in include_dirs for flag in ("-I", str(directory))),
                *(str(source) for source in sources),
                "-o", str(executable),
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
