#!/usr/bin/env python3
"""Create a secret-redacted unsigned firmware artifact bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
from datetime import datetime, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RESERVED_OUTPUT_NAMES = frozenset(
    {
        "FLASH-MANIFEST.json",
        "RELEASE-METADATA.json",
        "SHA256SUMS",
        "sdkconfig.redacted",
    }
)
RESERVED_OUTPUT_CASEFOLDS = frozenset(name.casefold() for name in RESERVED_OUTPUT_NAMES)
IMMUTABLE_BUILDER_IMAGE = re.compile(r"^[^\s@]+@sha256:[0-9a-f]{64}$")
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


def read_project_identity(path: Path) -> dict[str, object]:
    """Return the non-sensitive, path-free portion of ESP-IDF build metadata."""
    raw = json.loads(path.read_text(encoding="utf-8"))
    fields = (
        "project_name",
        "project_version",
        "git_revision",
        "target",
        "min_rev",
        "max_rev",
    )
    return {field: raw[field] for field in fields if field in raw}


def resolve_source_commit(explicit: str | None) -> str:
    candidate = explicit or os.getenv("GITHUB_SHA")
    if not candidate:
        try:
            completed = subprocess.run(
                ["git", "rev-parse", "HEAD"],
                cwd=ROOT,
                check=True,
                capture_output=True,
                text=True,
            )
            candidate = completed.stdout.strip()
        except (OSError, subprocess.CalledProcessError) as exc:
            raise ValueError("could not resolve the source commit") from exc
    if not re.fullmatch(
        r"(?:[0-9a-fA-F]{40}|[0-9a-fA-F]{64})", candidate
    ):
        raise ValueError("source commit must be a full hexadecimal Git object ID")
    candidate = candidate.lower()
    try:
        completed = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError) as exc:
        raise ValueError("could not verify the checked-out source commit") from exc
    if completed.stdout.strip().lower() != candidate:
        raise ValueError("source commit does not match the checked-out HEAD")
    return candidate


def source_tree_dirty() -> bool:
    try:
        completed = subprocess.run(
            ["git", "status", "--porcelain", "--untracked-files=all"],
            cwd=ROOT,
            check=True,
            capture_output=True,
        )
    except (OSError, subprocess.CalledProcessError) as exc:
        raise ValueError("could not determine whether the source tree is clean") from exc
    return bool(completed.stdout)


def current_project_version() -> str:
    try:
        completed = subprocess.run(
            ["git", "describe", "--always", "--dirty"],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError) as exc:
        raise ValueError("could not resolve the current project version") from exc
    return completed.stdout.strip()


def read_flash_plan(build_dir: Path) -> tuple[dict[str, object], dict[str, Path]]:
    """Flatten ESP-IDF's flash plan into a portable artifact manifest."""
    source = build_dir / "flasher_args.json"
    raw = json.loads(source.read_text(encoding="utf-8"))
    flash_files = raw.get("flash_files")
    if not isinstance(flash_files, dict) or not flash_files:
        raise ValueError("flasher_args.json contains no flash files")

    copied: dict[str, Path] = {}
    copied_casefolds: set[str] = set()
    portable_files: dict[str, str] = {}
    resolved_build = build_dir.resolve()
    for offset, relative_name in sorted(
        flash_files.items(), key=lambda item: int(str(item[0]), 0)
    ):
        if not isinstance(relative_name, str) or not relative_name:
            raise ValueError("flasher_args.json contains an invalid flash filename")
        output_name = Path(relative_name).name
        source_path = (resolved_build / relative_name).resolve()
        try:
            source_path.relative_to(resolved_build)
        except ValueError as exc:
            raise ValueError("flash input escapes the build directory") from exc
        if not source_path.is_file():
            raise ValueError(f"flash input is missing or not a file: {output_name}")
        output_casefold = output_name.casefold()
        if output_casefold in RESERVED_OUTPUT_CASEFOLDS:
            raise ValueError(f"flash input uses a reserved artifact name: {output_name}")
        if output_casefold in copied_casefolds:
            raise ValueError(f"flash input basename collision: {output_name}")
        copied[output_name] = source_path
        copied_casefolds.add(output_casefold)
        portable_files[str(offset)] = output_name

    extra = raw.get("extra_esptool_args", {})
    if not isinstance(extra, dict):
        extra = {}
    manifest = {
        "schema_version": 1,
        "chip": extra.get("chip", "unknown"),
        "write_flash_args": raw.get("write_flash_args", []),
        "flash_files": portable_files,
        "flash_settings": raw.get("flash_settings", {}),
        "reset": {
            "before": extra.get("before", "default-reset"),
            "after": extra.get("after", "hard-reset"),
        },
    }
    return manifest, copied


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--configuration", required=True)
    parser.add_argument(
        "--source-commit",
        help="full source commit (defaults to GITHUB_SHA or the current Git HEAD)",
    )
    parser.add_argument(
        "--release-version",
        help="immutable release identifier (defaults to the ESP-IDF project version)",
    )
    parser.add_argument(
        "--builder-image",
        default=os.getenv("ESP_IDF_BUILDER_IMAGE", "unknown"),
        help="immutable builder image reference recorded as provenance",
    )
    parser.add_argument(
        "--require-clean-source",
        action="store_true",
        help="reject dirty source or a build version that does not match this checkout",
    )
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
        "dshot_esc.elf": build_dir / "dshot_esc.elf",
        "dshot_esc.map": build_dir / "dshot_esc.map",
        "partitions.csv": ROOT / "partitions.csv",
        "size-gate.json": build_dir / "size-gate.json",
        "dependencies.lock": ROOT / "dependencies.lock",
    }
    project_description = build_dir / "project_description.json"
    flasher_args = build_dir / "flasher_args.json"
    missing = [
        source
        for source in [*inputs.values(), sdkconfig, project_description, flasher_args]
        if not source.is_file()
    ]
    if missing:
        parser.error("required release input(s) missing: " + ", ".join(str(path) for path in missing))
    if output_dir.exists():
        parser.error(f"output directory must not already exist: {output_dir}")
    try:
        flash_manifest, flash_inputs = read_flash_plan(build_dir)
        input_names = {name.casefold() for name in inputs}
        collisions = sorted(
            name for name in flash_inputs if name.casefold() in input_names
        )
        if collisions:
            raise ValueError(
                "flash input collides with a release artifact name: "
                + ", ".join(collisions)
            )
        source_commit = resolve_source_commit(args.source_commit)
        dirty = source_tree_dirty()
        checkout_version = current_project_version()
        project_identity = read_project_identity(project_description)
        build_version_matches_source = (
            project_identity.get("project_version") == checkout_version
        )
        if args.require_clean_source and dirty:
            raise ValueError("source tree is dirty")
        if args.require_clean_source and not build_version_matches_source:
            raise ValueError("build project version does not match the checked-out source")
        if args.require_clean_source and not IMMUTABLE_BUILDER_IMAGE.fullmatch(
            args.builder_image
        ):
            raise ValueError("builder image must use an immutable sha256 digest")
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        parser.error(str(exc))

    output_dir.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix=f".{output_dir.name}.tmp-",
        dir=output_dir.parent,
        ignore_cleanup_errors=True,
    ) as staging_name:
        staging_dir = Path(staging_name)
        for name, source in {**inputs, **flash_inputs}.items():
            copy_required(source, staging_dir / name)
        redact_sdkconfig(sdkconfig, staging_dir / "sdkconfig.redacted")

        flash_manifest["files"] = [
            {
                "offset": offset,
                "file": filename,
                "sha256": sha256(staging_dir / filename),
            }
            for offset, filename in flash_manifest["flash_files"].items()
        ]
        (staging_dir / "FLASH-MANIFEST.json").write_text(
            json.dumps(flash_manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
            newline="\n",
        )

        metadata = {
            "schema_version": 2,
            "generated_at": datetime.now(timezone.utc).isoformat(),
            "configuration": args.configuration,
            "release_version": args.release_version
            or project_identity.get("project_version", "unknown"),
            "source_commit": source_commit,
            "source_repository": os.getenv("GITHUB_REPOSITORY", "unknown"),
            "builder_image": args.builder_image,
            "source_tree_dirty": dirty,
            "build_version_matches_source": build_version_matches_source,
            "build": project_identity,
            "redacted_configuration_sha256": sha256(
                staging_dir / "sdkconfig.redacted"
            ),
            "flash_manifest": "FLASH-MANIFEST.json",
            "signing": "unsigned",
            "production_ota_suitable": False,
            "warning": "UNSIGNED CI BUILD - NOT FOR PRODUCTION OTA",
        }
        (staging_dir / "RELEASE-METADATA.json").write_text(
            json.dumps(metadata, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
            newline="\n",
        )

        hashes = []
        for path in sorted(staging_dir.iterdir(), key=lambda item: item.name):
            if path.is_file() and path.name != "SHA256SUMS":
                hashes.append(f"{sha256(path)}  {path.name}")
        (staging_dir / "SHA256SUMS").write_text(
            "\n".join(hashes) + "\n", encoding="ascii", newline="\n"
        )
        staging_dir.replace(output_dir)
    print(f"Packaged {len(hashes)} unsigned artifacts in {output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
