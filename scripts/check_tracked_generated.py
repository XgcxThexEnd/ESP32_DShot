#!/usr/bin/env python3
"""Reject generated artifacts and missing release inputs in the Git index."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path, PurePosixPath


ROOT = Path(__file__).resolve().parents[1]
REQUIRED_TRACKED = {
    PurePosixPath("dependencies.lock"),
    PurePosixPath("main/idf_component.yml"),
    PurePosixPath("partitions.csv"),
    PurePosixPath("sdkconfig.defaults"),
    PurePosixPath("configs/sdkconfig.dev.defaults"),
    PurePosixPath("configs/sdkconfig.production.defaults"),
    PurePosixPath("configs/sdkconfig.single-fan-node.defaults"),
    PurePosixPath("configs/sdkconfig.ci-safe.defaults"),
    PurePosixPath("configs/sdkconfig.ci-max-four-fan.defaults"),
    PurePosixPath("configs/sdkconfig.ci-optional-features.defaults"),
    PurePosixPath("configs/sdkconfig.ci-mqtt-restart.defaults"),
    PurePosixPath("configs/sdkconfig.ci-legacy.defaults"),
}
BLOCKED_EXACT = {
    PurePosixPath("sdkconfig"),
    PurePosixPath("sdkconfig.old"),
}
BLOCKED_PARTS = {"__pycache__", ".pytest_cache", "managed_components"}
BLOCKED_SUFFIXES = {".bin", ".elf", ".map", ".pyc", ".pyo"}


def tracked_paths() -> set[PurePosixPath]:
    result = subprocess.run(
        ["git", "ls-files", "-z"],
        cwd=ROOT,
        check=True,
        stdout=subprocess.PIPE,
    )
    return {
        PurePosixPath(raw.decode("utf-8", errors="surrogateescape"))
        for raw in result.stdout.split(b"\0")
        if raw
    }


def blocked_reason(path: PurePosixPath) -> str | None:
    if path in BLOCKED_EXACT:
        return "generated sdkconfig"
    if path.parts and path.parts[0].startswith("build"):
        return "ESP-IDF build output"
    if any(part in BLOCKED_PARTS for part in path.parts):
        return "generated dependency or cache directory"
    if path.suffix.lower() in BLOCKED_SUFFIXES:
        return "generated binary/cache file"
    return None


def main() -> int:
    try:
        tracked = tracked_paths()
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"could not inspect the Git index: {exc}", file=sys.stderr)
        return 2

    findings: list[str] = []
    for path in sorted(tracked, key=str):
        reason = blocked_reason(path)
        if reason:
            findings.append(f"tracked generated file: {path.as_posix()} ({reason})")

    for path in sorted(REQUIRED_TRACKED - tracked, key=str):
        findings.append(f"required reproducibility input is not tracked: {path.as_posix()}")

    if findings:
        print("Tracked-file policy failed:")
        for finding in findings:
            print(f"- {finding}")
        return 1

    print(
        f"Tracked-file policy passed ({len(tracked)} files; "
        f"{len(REQUIRED_TRACKED)} required inputs present)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
