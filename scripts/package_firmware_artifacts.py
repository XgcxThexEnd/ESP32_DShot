#!/usr/bin/env python3
"""Create a secret-redacted unsigned firmware artifact bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
from datetime import datetime, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SENSITIVE_CONFIG = re.compile(
    r"^(CONFIG_[A-Z0-9_]*(?:PASSWORD|PASS|SSID|USERNAME|BROKER_URI|SECRET|TOKEN|"
    r"PRIVATE_KEY|SIGNING_KEY|CLIENT_KEY)[A-Z0-9_]*)=.*$"
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def redact_sdkconfig(source: Path, destination: Path) -> None:
    lines = []
    for line in source.read_text(encoding="utf-8").splitlines():
        match = SENSITIVE_CONFIG.match(line)
        lines.append(f'{match.group(1)}="<redacted>"' if match else line)
    destination.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def copy_required(source: Path, destination: Path) -> None:
    if not source.is_file():
        raise FileNotFoundError(f"required release input is missing: {source}")
    shutil.copy2(source, destination)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--configuration", required=True)
    parser.add_argument(
        "--sdkconfig",
        type=Path,
        help="resolved sdkconfig (defaults to <build-dir>/sdkconfig)",
    )
    args = parser.parse_args()

    build_dir = args.build_dir.resolve()
    output_dir = args.output_dir.resolve()
    sdkconfig = args.sdkconfig.resolve() if args.sdkconfig else build_dir / "sdkconfig"

    inputs = {
        "dshot_esc.bin": build_dir / "dshot_esc.bin",
        "dshot_esc.elf": build_dir / "dshot_esc.elf",
        "dshot_esc.map": build_dir / "dshot_esc.map",
        "partition-table.bin": build_dir / "partition_table" / "partition-table.bin",
        "partitions.csv": ROOT / "partitions.csv",
        "size-gate.json": build_dir / "size-gate.json",
        "dependencies.lock": ROOT / "dependencies.lock",
    }
    missing = [source for source in [*inputs.values(), sdkconfig] if not source.is_file()]
    if missing:
        parser.error("required release input(s) missing: " + ", ".join(str(path) for path in missing))
    if output_dir.exists() and any(output_dir.iterdir()):
        parser.error(f"output directory must be empty: {output_dir}")
    output_dir.mkdir(parents=True, exist_ok=True)
    for name, source in inputs.items():
        copy_required(source, output_dir / name)
    redact_sdkconfig(sdkconfig, output_dir / "sdkconfig.redacted")

    metadata = {
        "schema_version": 1,
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "configuration": args.configuration,
        "signing": "unsigned",
        "production_ota_suitable": False,
        "warning": "UNSIGNED CI BUILD - NOT FOR PRODUCTION OTA",
    }
    (output_dir / "RELEASE-METADATA.json").write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="\n"
    )

    hashes = []
    for path in sorted(output_dir.iterdir(), key=lambda item: item.name):
        if path.is_file() and path.name != "SHA256SUMS":
            hashes.append(f"{sha256(path)}  {path.name}")
    (output_dir / "SHA256SUMS").write_text(
        "\n".join(hashes) + "\n", encoding="ascii", newline="\n"
    )
    print(f"Packaged {len(hashes)} unsigned artifacts in {output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
