#!/usr/bin/env python3
"""Enforce application-partition headroom for factory and OTA images."""

from __future__ import annotations

import argparse
import csv
import json
import math
from dataclasses import asdict, dataclass
from pathlib import Path


@dataclass(frozen=True)
class AppPartition:
    name: str
    subtype: str
    offset: int
    size: int


def parse_size(value: str) -> int:
    text = value.strip().lower()
    multiplier = 1
    if text.endswith("k"):
        multiplier = 1024
        text = text[:-1]
    elif text.endswith("m"):
        multiplier = 1024 * 1024
        text = text[:-1]
    parsed = int(text, 0)
    if parsed < 0:
        raise ValueError("partition values must not be negative")
    return parsed * multiplier


def read_app_partitions(path: Path) -> tuple[list[AppPartition], bool]:
    partitions: list[AppPartition] = []
    has_otadata = False
    with path.open("r", encoding="utf-8", newline="") as handle:
        for row in csv.reader(line for line in handle if not line.lstrip().startswith("#")):
            if not row or all(not column.strip() for column in row):
                continue
            if len(row) < 5:
                raise ValueError(f"partition row has fewer than five fields: {row!r}")
            name, part_type, subtype, offset, size = (column.strip() for column in row[:5])
            if part_type == "data" and subtype == "ota":
                has_otadata = True
            if part_type != "app":
                continue
            partitions.append(
                AppPartition(
                    name=name,
                    subtype=subtype,
                    offset=parse_size(offset),
                    size=parse_size(size),
                )
            )
    return partitions, has_otadata


def evaluate_image(
    image_size: int,
    partitions: list[AppPartition],
    *,
    maximum_utilization: float,
    minimum_free_bytes: int,
) -> dict[str, object]:
    if image_size <= 0:
        raise ValueError("application image must not be empty")
    if not partitions:
        raise ValueError("partition table has no application partitions")
    if not 0.0 < maximum_utilization < 1.0:
        raise ValueError("maximum utilization must be between zero and one")
    if minimum_free_bytes < 0:
        raise ValueError("minimum free bytes must not be negative")

    slots: list[dict[str, object]] = []
    failures: list[str] = []
    for partition in partitions:
        free_bytes = partition.size - image_size
        utilization = image_size / partition.size if partition.size else math.inf
        slot = {
            **asdict(partition),
            "image_size": image_size,
            "free_bytes": free_bytes,
            "utilization": utilization,
        }
        slots.append(slot)
        if image_size > partition.size:
            failures.append(f"{partition.name}: image exceeds partition by {-free_bytes} bytes")
        if utilization > maximum_utilization:
            failures.append(
                f"{partition.name}: {utilization:.2%} utilization exceeds "
                f"{maximum_utilization:.2%}"
            )
        if free_bytes < minimum_free_bytes:
            failures.append(
                f"{partition.name}: {free_bytes} free bytes is below the "
                f"{minimum_free_bytes}-byte margin"
            )
    return {"image_size": image_size, "slots": slots, "failures": failures}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--partition-table", type=Path, default=Path("partitions.csv"))
    parser.add_argument("--app", type=Path, required=True, help="built application .bin")
    parser.add_argument("--maximum-utilization", type=float, default=0.85)
    parser.add_argument("--minimum-free-bytes", type=int, default=128 * 1024)
    parser.add_argument("--json-output", type=Path)
    args = parser.parse_args()

    try:
        partitions, has_otadata = read_app_partitions(args.partition_table)
        report = evaluate_image(
            args.app.stat().st_size,
            partitions,
            maximum_utilization=args.maximum_utilization,
            minimum_free_bytes=args.minimum_free_bytes,
        )
    except (OSError, ValueError) as exc:
        parser.error(str(exc))

    report["partition_table"] = str(args.partition_table)
    report["app"] = str(args.app)
    report["has_otadata"] = has_otadata
    failures = report["failures"]
    assert isinstance(failures, list)
    ota_slots = [part for part in partitions if part.subtype.startswith("ota_")]
    if len(ota_slots) < 2:
        failures.append("partition table must contain at least two OTA application slots")
    if not has_otadata:
        failures.append("partition table is missing the OTA data partition")

    for slot in report["slots"]:
        assert isinstance(slot, dict)
        print(
            f"{slot['name']}: image={slot['image_size']} size={slot['size']} "
            f"free={slot['free_bytes']} utilization={slot['utilization']:.2%}"
        )
    if args.json_output:
        args.json_output.parent.mkdir(parents=True, exist_ok=True)
        args.json_output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    if failures:
        print("Firmware size/partition gate failed:")
        for failure in failures:
            print(f"- {failure}")
        return 1
    print("Firmware size/partition gate passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
