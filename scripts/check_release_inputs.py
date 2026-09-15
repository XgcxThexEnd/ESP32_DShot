#!/usr/bin/env python3
"""Reject release SDK, chip, and builder drift before compiling firmware."""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def check_inputs(contract: dict, lock: str, workflow: str) -> list[str]:
    errors = []
    expected = {
        "ESP-IDF version": (r"^  idf:\n(?:(?:    .*|)\n)*?    version: (\S+)$", contract["esp_idf_version"]),
        "chip target": (r"^target: (\S+)$", contract["target"]),
        "lock schema": (r"^version: (\S+)$", contract["lock_schema"]),
    }
    for label, (pattern, value) in expected.items():
        match = re.search(pattern, lock, re.MULTILINE)
        if not match or match[1] != value:
            errors.append(f"dependency lock {label} must be {value}")
    images = re.findall(r"^\s*(?:image|ESP_IDF_BUILDER_IMAGE):\s*(\S+)\s*$", workflow, re.MULTILINE)
    if len(images) != 2 or any(image != contract["builder_image"] for image in images):
        errors.append("CI container and artifact builder identity must match release-target.json")
    targets = re.findall(r"\bset-target\s+(\S+)", workflow)
    if targets != [contract["target"]]:
        errors.append("CI build target must match release-target.json")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args()
    root = args.root
    try:
        errors = check_inputs(
            json.loads((root / "configs/release-target.json").read_text()),
            (root / "dependencies.lock").read_text(),
            (root / ".github/workflows/ci.yml").read_text(),
        )
    except (OSError, ValueError, KeyError) as exc:
        print(f"Release input check failed: {exc}")
        return 1
    for error in errors:
        print(f"Release input check failed: {error}")
    if not errors:
        print("Release SDK, target, lock schema, and builder image agree.")
    return int(bool(errors))


if __name__ == "__main__":
    raise SystemExit(main())
