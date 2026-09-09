#!/usr/bin/env python3
"""Validate a hardware-in-loop release evidence manifest and its artifacts."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from datetime import datetime
from pathlib import Path, PurePosixPath
from typing import Any


SCHEMA = "esp32-dshot-hil-evidence/v1"
REQUIRED_CHECK_IDS = frozenset(
    {
        "dshot_timing",
        "protocol_safety",
        "startup_zero",
        "channel_isolation",
        "interlock_fail_safe",
        "tach_feedback",
        "network_recovery",
        "ota_rollback",
        "power_ordering",
        "soak",
    }
)
REQUIRED_ARTIFACT_KINDS = {
    "boot-dshot-capture": "logic_analyzer_csv",
    "steady-dshot-capture": "logic_analyzer_csv",
    "interlock-capture": "oscilloscope_capture",
    "tach-comparison": "tach_reference_csv",
    "network-recovery-log": "mqtt_and_serial_log",
    "ota-rollback-log": "serial_log",
    "power-ordering-capture": "oscilloscope_capture",
    "soak-log": "soak_csv",
}
REQUIRED_CHECK_ARTIFACTS = {
    "dshot_timing": frozenset({"steady-dshot-capture"}),
    "protocol_safety": frozenset(
        {"boot-dshot-capture", "steady-dshot-capture"}
    ),
    "startup_zero": frozenset({"boot-dshot-capture"}),
    "channel_isolation": frozenset({"steady-dshot-capture"}),
    "interlock_fail_safe": frozenset({"interlock-capture"}),
    "tach_feedback": frozenset({"tach-comparison"}),
    "network_recovery": frozenset({"network-recovery-log"}),
    "ota_rollback": frozenset({"ota-rollback-log"}),
    "power_ordering": frozenset({"power-ordering-capture"}),
    "soak": frozenset({"soak-log"}),
}
# This repository deliberately fixes the production partition layout. Binding
# these offsets prevents a truncated app-only flash plan from masquerading as
# the complete image set that was exercised on the bench.
REQUIRED_FLASH_LAYOUT = {
    0x0: "bootloader.bin",
    0x8000: "partition-table.bin",
    0x3D0000: "ota_data_initial.bin",
}
APPLICATION_FLASH_OFFSET = 0x10000
CONDITIONAL_CHECK_IDS = frozenset(
    {"interlock_fail_safe", "tach_feedback", "ota_rollback"}
)
VALID_STATUSES = frozenset({"pass", "fail", "not_applicable", "not_run"})
IDENTIFIER = re.compile(r"[a-z0-9][a-z0-9_-]*\Z")
NODE_IDENTIFIER = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]{0,47}\Z")
GIT_OBJECT_ID = re.compile(r"(?:[0-9a-f]{40}|[0-9a-f]{64})\Z")
HEX_64 = re.compile(r"[0-9a-f]{64}\Z")
HEX_16 = re.compile(r"[0-9a-f]{16}\Z")
SDKCONFIG_ASSIGNMENT = re.compile(r"^(CONFIG_[A-Z0-9_]+)=(.*)$")
SDKCONFIG_NOT_SET = re.compile(r"^# (CONFIG_[A-Z0-9_]+) is not set$")
SENSITIVE_CONFIG_SYMBOL = re.compile(
    r"^CONFIG_[A-Z0-9_]*(?:PASSWORD|PASS|SSID|USERNAME|BROKER_URI|SECRET|TOKEN|"
    r"PRIVATE_KEY|SIGNING_KEY|CLIENT_KEY)[A-Z0-9_]*$"
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _mapping(value: Any, label: str, errors: list[str]) -> dict[str, Any]:
    if not isinstance(value, dict):
        errors.append(f"{label} must be an object")
        return {}
    return value


def _sequence(value: Any, label: str, errors: list[str]) -> list[Any]:
    if not isinstance(value, list):
        errors.append(f"{label} must be an array")
        return []
    return value


def _required_string(
    value: Any,
    label: str,
    errors: list[str],
    *,
    allow_incomplete: bool,
) -> str | None:
    if not isinstance(value, str) or not value.strip():
        errors.append(f"{label} must be a non-empty string")
        return None
    if not allow_incomplete and value.strip().upper().startswith("REPLACE_"):
        errors.append(f"{label} still contains a template placeholder")
    return value


def _positive_number(
    value: Any,
    label: str,
    errors: list[str],
    *,
    allow_incomplete: bool,
) -> None:
    if allow_incomplete and value is None:
        return
    if isinstance(value, bool) or not isinstance(value, (int, float)) or value <= 0:
        errors.append(f"{label} must be a positive number")


def _gpio_list(value: Any, label: str, errors: list[str]) -> list[int]:
    raw = _sequence(value, label, errors)
    gpios: list[int] = []
    for index, gpio in enumerate(raw):
        if isinstance(gpio, bool) or not isinstance(gpio, int) or not 0 <= gpio <= 48:
            errors.append(f"{label}[{index}] must be an ESP32-S3 GPIO number (0..48)")
            continue
        gpios.append(gpio)
    if len(set(gpios)) != len(gpios):
        errors.append(f"{label} contains a duplicate GPIO")
    return gpios


def _safe_artifact_path(value: Any, label: str, errors: list[str]) -> PurePosixPath | None:
    if not isinstance(value, str) or not value or "\\" in value:
        errors.append(f"{label} must be a non-empty POSIX relative path")
        return None
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in {"", ".", ".."} for part in path.parts):
        errors.append(f"{label} must remain inside the evidence directory")
        return None
    return path


def _validate_timestamp(value: Any, errors: list[str], allow_incomplete: bool) -> None:
    if allow_incomplete and value is None:
        return
    if not isinstance(value, str):
        errors.append("release.tested_at_utc must be an RFC 3339 timestamp")
        return
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        errors.append("release.tested_at_utc must be an RFC 3339 timestamp")
        return
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        errors.append("release.tested_at_utc must include a UTC offset")


def _hashed_evidence_file(
    container: dict[str, Any],
    stem: str,
    evidence_root: Path,
    errors: list[str],
    *,
    allow_incomplete: bool,
) -> Path | None:
    path_label = f"release.{stem}_path"
    digest_label = f"release.{stem}_sha256"
    relative = _safe_artifact_path(container.get(f"{stem}_path"), path_label, errors)
    expected = container.get(f"{stem}_sha256")
    digest_valid = isinstance(expected, str) and HEX_64.fullmatch(expected)
    if not digest_valid:
        errors.append(f"{digest_label} must be 64 lowercase hexadecimal characters")
    elif expected == "0" * 64 and not allow_incomplete:
        errors.append(f"{digest_label} still contains the template digest")
    if relative is None or allow_incomplete:
        return None
    path = evidence_root.joinpath(*relative.parts).resolve()
    try:
        path.relative_to(evidence_root)
    except ValueError:
        errors.append(f"{path_label} resolves outside the evidence directory")
        return None
    if not path.is_file():
        errors.append(f"{path_label} does not exist: {relative.as_posix()}")
        return None
    if digest_valid and expected != "0" * 64 and sha256(path) != expected:
        errors.append(f"{digest_label} does not match {relative.as_posix()}")
    return path


def _read_sdkconfig(path: Path, errors: list[str]) -> dict[str, str]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError):
        errors.append("release.sdkconfig_redacted_path is not readable text")
        return {}
    values: dict[str, str] = {}
    for line_number, line in enumerate(lines, start=1):
        assignment = SDKCONFIG_ASSIGNMENT.fullmatch(line)
        unset = SDKCONFIG_NOT_SET.fullmatch(line)
        if assignment:
            symbol, value = assignment.groups()
        elif unset:
            symbol, value = unset.group(1), "n"
        elif line.startswith("CONFIG_") or line.startswith("# CONFIG_"):
            errors.append(
                "release sdkconfig contains a malformed CONFIG entry "
                f"at line {line_number}"
            )
            continue
        else:
            continue
        if symbol in values and values[symbol] != value:
            errors.append(f"release sdkconfig contains conflicting {symbol}")
        values[symbol] = value
    return values


def _validate_redacted_sdkconfig(
    values: dict[str, str], errors: list[str]
) -> None:
    for symbol, raw_value in values.items():
        if not SENSITIVE_CONFIG_SYMBOL.fullmatch(symbol):
            continue
        decoded = _config_value(values, symbol)
        if raw_value != "n" and decoded not in {"", "<redacted>"}:
            errors.append(
                f"release sdkconfig contains a populated sensitive value: {symbol}"
            )


def _flash_offset(value: Any, label: str, errors: list[str]) -> int | None:
    if not isinstance(value, str) or not re.fullmatch(
        r"(?:0[xX][0-9a-fA-F]+|0|[1-9][0-9]*)", value
    ):
        errors.append(f"{label} must be a non-negative integer offset string")
        return None
    parsed = int(value, 16 if value.lower().startswith("0x") else 10)
    if parsed > 0xFFFFFFFF:
        errors.append(f"{label} exceeds the supported 32-bit flash address range")
        return None
    return parsed


def _flash_filename(value: Any, label: str, errors: list[str]) -> str | None:
    relative = _safe_artifact_path(value, label, errors)
    if relative is None:
        return None
    if len(relative.parts) != 1:
        errors.append(f"{label} must be a packaged file basename")
        return None
    return relative.name


def _validate_flash_manifest(
    flash_path: Path,
    firmware_path: Path,
    expected_firmware_digest: Any,
    evidence_root: Path,
    errors: list[str],
) -> None:
    try:
        flash = json.loads(flash_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        errors.append("release.flash_manifest_path is not readable JSON")
        return
    if not isinstance(flash, dict) or flash.get("schema_version") != 1:
        errors.append("flash manifest must use schema_version 1")
        return

    raw_flash_files = flash.get("flash_files")
    if not isinstance(raw_flash_files, dict) or not raw_flash_files:
        errors.append("flash manifest must contain a non-empty flash_files object")
        raw_flash_files = {}
    declared: dict[int, tuple[str, str]] = {}
    declared_names: set[str] = set()
    for raw_offset, raw_filename in raw_flash_files.items():
        label = f"flash manifest flash_files[{raw_offset!r}]"
        offset = _flash_offset(raw_offset, f"{label} offset", errors)
        filename = _flash_filename(raw_filename, f"{label} filename", errors)
        if offset is None or filename is None:
            continue
        if offset in declared:
            errors.append(f"flash manifest contains duplicate offset {raw_offset!r}")
            continue
        folded_name = filename.casefold()
        if folded_name in declared_names:
            errors.append(f"flash manifest contains duplicate filename: {filename}")
            continue
        declared[offset] = (raw_offset, filename)
        declared_names.add(folded_name)

    required_layout = {
        **REQUIRED_FLASH_LAYOUT,
        APPLICATION_FLASH_OFFSET: firmware_path.name,
    }
    for offset, expected_filename in required_layout.items():
        declared_entry = declared.get(offset)
        if declared_entry is None:
            errors.append(
                f"flash manifest is missing required file {expected_filename} "
                f"at {offset:#x}"
            )
        elif declared_entry[1] != expected_filename:
            errors.append(
                f"flash manifest must place {expected_filename} at {offset:#x}"
            )

    raw_entries = flash.get("files")
    if not isinstance(raw_entries, list) or not raw_entries:
        errors.append("flash manifest must contain a non-empty files array")
        raw_entries = []
    entries: dict[int, tuple[str, str, str]] = {}
    entry_names: set[str] = set()
    for index, raw_entry in enumerate(raw_entries):
        label = f"flash manifest files[{index}]"
        if not isinstance(raw_entry, dict):
            errors.append(f"{label} must be an object")
            continue
        raw_offset = raw_entry.get("offset")
        offset = _flash_offset(raw_offset, f"{label}.offset", errors)
        filename = _flash_filename(raw_entry.get("file"), f"{label}.file", errors)
        digest = raw_entry.get("sha256")
        if not isinstance(digest, str) or not HEX_64.fullmatch(digest):
            errors.append(f"{label}.sha256 must be 64 lowercase hexadecimal characters")
            digest = ""
        elif digest == "0" * 64:
            errors.append(f"{label}.sha256 cannot be the all-zero template digest")
        if offset is None or filename is None:
            continue
        if offset in entries:
            errors.append(f"flash manifest files contains duplicate offset {raw_offset!r}")
            continue
        folded_name = filename.casefold()
        if folded_name in entry_names:
            errors.append(f"flash manifest files contains duplicate filename: {filename}")
            continue
        entries[offset] = (raw_offset, filename, digest)
        entry_names.add(folded_name)

        packaged_path = (flash_path.parent / filename).resolve()
        try:
            packaged_path.relative_to(evidence_root)
        except ValueError:
            errors.append(f"{label}.file resolves outside the evidence directory")
            continue
        if not packaged_path.is_file():
            errors.append(f"{label}.file does not exist beside the flash manifest: {filename}")
        elif digest and digest != "0" * 64 and sha256(packaged_path) != digest:
            errors.append(f"{label}.sha256 does not match {filename}")

    for offset, (offset_text, filename) in declared.items():
        entry = entries.get(offset)
        if entry is None:
            errors.append(
                f"flash manifest offset {offset_text!r} is missing from the files array"
            )
        elif entry[0] != offset_text or entry[1] != filename:
            errors.append(
                f"flash manifest files entry does not match flash_files at {offset_text!r}"
            )
    for offset, (offset_text, _, _) in entries.items():
        if offset not in declared:
            errors.append(
                f"flash manifest files offset {offset_text!r} is absent from flash_files"
            )

    matching_firmware = [
        entry
        for entry in entries.values()
        if entry[1] == firmware_path.name
        and entry[2] == expected_firmware_digest
    ]
    if len(matching_firmware) != 1:
        errors.append("flash manifest does not bind the tested firmware digest")


def _config_value(values: dict[str, str], symbol: str) -> Any:
    raw = values.get(symbol)
    if raw is None:
        return None
    if len(raw) >= 2 and raw[0] == '"' and raw[-1] == '"':
        try:
            return json.loads(raw)
        except json.JSONDecodeError:
            return raw
    try:
        return int(raw, 0)
    except ValueError:
        return raw


def _config_gpio_list(values: dict[str, str], symbol: str) -> list[int] | None:
    decoded = _config_value(values, symbol)
    if not isinstance(decoded, str):
        return None
    try:
        return [int(item.strip(), 10) for item in decoded.split(",")]
    except ValueError:
        return None


def topology_fingerprint_from_sdkconfig(
    values: dict[str, str], node_id_override: str | None = None
) -> str | None:
    configured_node_id = _config_value(values, "CONFIG_NODE_ID")
    node_id = configured_node_id or node_id_override
    fan_count = _config_value(values, "CONFIG_FAN_COUNT")
    fan_start = _config_value(values, "CONFIG_FAN_INDEX_START")
    dshot_gpios = _config_gpio_list(values, "CONFIG_DSHOT_GPIO")
    tach_enabled = values.get("CONFIG_TACH_FEEDBACK_ENABLED") == "y"
    tach_gpios = _config_gpio_list(values, "CONFIG_TACH_GPIO") if tach_enabled else []
    if (
        not isinstance(node_id, str)
        or not node_id
        or isinstance(fan_count, bool)
        or not isinstance(fan_count, int)
        or not 1 <= fan_count <= 4
        or isinstance(fan_start, bool)
        or not isinstance(fan_start, int)
        or not 0 <= fan_start <= 0xFFFFFFFF
        or dshot_gpios is None
        or len(dshot_gpios) != fan_count
        or any(not 0 <= gpio <= 48 for gpio in dshot_gpios)
        or (
            tach_enabled
            and (
                tach_gpios is None
                or len(tach_gpios) != fan_count
                or any(not 0 <= gpio <= 48 for gpio in tach_gpios)
            )
        )
    ):
        # A blank node ID is MAC-derived on the device and cannot be reproduced
        # from a release bundle alone; the recorded runtime value remains the gate.
        return None

    value = 14695981039346656037

    def consume(data: bytes) -> None:
        nonlocal value
        for byte in data:
            value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF

    consume(b"ESP32_DShot/topology/v1")
    encoded_node = node_id.encode("utf-8")
    consume(len(encoded_node).to_bytes(4, "little"))
    consume(encoded_node)
    consume(fan_count.to_bytes(4, "little", signed=False))
    consume(fan_start.to_bytes(4, "little", signed=False))
    for gpio in dshot_gpios:
        consume(gpio.to_bytes(4, "little", signed=False))
    consume(bytes([1 if tach_enabled else 0]))
    for gpio in tach_gpios:
        consume(gpio.to_bytes(4, "little", signed=False))
    return f"{value:016x}"


def validate_manifest(
    manifest: Any,
    manifest_path: Path,
    *,
    allow_incomplete: bool = False,
) -> list[str]:
    """Return all validation failures without mutating the evidence bundle."""

    errors: list[str] = []
    root = _mapping(manifest, "manifest", errors)
    evidence_root = manifest_path.resolve().parent
    if root.get("schema") != SCHEMA:
        errors.append(f"schema must be {SCHEMA!r}")
    status = root.get("record_status")
    if status not in {"draft", "complete"}:
        errors.append("record_status must be 'draft' or 'complete'")
    elif status != "complete" and not allow_incomplete:
        errors.append("record_status must be 'complete' for release acceptance")

    release = _mapping(root.get("release"), "release", errors)
    commit = release.get("firmware_commit")
    if not isinstance(commit, str) or not GIT_OBJECT_ID.fullmatch(commit):
        errors.append(
            "release.firmware_commit must be 40 or 64 lowercase hexadecimal characters"
        )
    elif set(commit) == {"0"} and not allow_incomplete:
        errors.append("release.firmware_commit still contains the template digest")
    configuration = _required_string(
        release.get("configuration"),
        "release.configuration",
        errors,
        allow_incomplete=allow_incomplete,
    )
    node_id = _required_string(
        release.get("node_id"),
        "release.node_id",
        errors,
        allow_incomplete=allow_incomplete,
    )
    if isinstance(node_id, str) and not NODE_IDENTIFIER.fullmatch(node_id):
        errors.append("release.node_id must be a 1..48 character node identifier")
    esp_idf_version = _required_string(
        release.get("esp_idf_version"),
        "release.esp_idf_version",
        errors,
        allow_incomplete=allow_incomplete,
    )
    topology = release.get("topology_fingerprint")
    if not isinstance(topology, str) or not HEX_16.fullmatch(topology):
        errors.append(
            "release.topology_fingerprint must be 16 lowercase hexadecimal characters"
        )
    elif topology == "0" * 16 and not allow_incomplete:
        errors.append("release.topology_fingerprint still contains the template value")

    release_files = {
        stem: _hashed_evidence_file(
            release,
            stem,
            evidence_root,
            errors,
            allow_incomplete=allow_incomplete,
        )
        for stem in (
            "firmware",
            "release_metadata",
            "flash_manifest",
            "sdkconfig_redacted",
        )
    }
    release_paths = [path for path in release_files.values() if path is not None]
    if len(set(release_paths)) != len(release_paths):
        errors.append("release evidence paths must identify distinct files")

    resolved_config: dict[str, str] = {}
    sdkconfig_path = release_files["sdkconfig_redacted"]
    if sdkconfig_path is not None and not allow_incomplete:
        resolved_config = _read_sdkconfig(sdkconfig_path, errors)
        if not resolved_config:
            errors.append(
                "release.sdkconfig_redacted_path contains no parseable sdkconfig entries"
            )
        else:
            _validate_redacted_sdkconfig(resolved_config, errors)

    metadata_path = release_files["release_metadata"]
    if metadata_path is not None and not allow_incomplete:
        try:
            metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError):
            errors.append("release.release_metadata_path is not readable JSON")
        else:
            if not isinstance(metadata, dict) or metadata.get("schema_version") != 2:
                errors.append("release metadata must use schema_version 2")
            else:
                if metadata.get("source_commit") != commit:
                    errors.append("release metadata source_commit does not match the manifest")
                if metadata.get("configuration") != configuration:
                    errors.append("release metadata configuration does not match the manifest")
                expected_flash_manifest = release_files["flash_manifest"]
                if (
                    expected_flash_manifest is not None
                    and metadata.get("flash_manifest")
                    != expected_flash_manifest.name
                ):
                    errors.append("release metadata flash manifest does not match the manifest")
                build = metadata.get("build")
                if (
                    not isinstance(build, dict)
                    or build.get("git_revision") != esp_idf_version
                ):
                    errors.append("release metadata ESP-IDF version does not match the manifest")
                if metadata.get("redacted_configuration_sha256") != release.get(
                    "sdkconfig_redacted_sha256"
                ):
                    errors.append(
                        "release metadata configuration digest does not match the manifest"
                    )
                if metadata.get("source_tree_dirty") is not False:
                    errors.append("release metadata must prove a clean source tree")
                if metadata.get("build_version_matches_source") is not True:
                    errors.append("release metadata build version is not source-bound")

    flash_path = release_files["flash_manifest"]
    firmware_path = release_files["firmware"]
    if flash_path is not None and firmware_path is not None and not allow_incomplete:
        _validate_flash_manifest(
            flash_path,
            firmware_path,
            release.get("firmware_sha256"),
            evidence_root,
            errors,
        )
    _validate_timestamp(release.get("tested_at_utc"), errors, allow_incomplete)

    hardware = _mapping(root.get("hardware"), "hardware", errors)
    _required_string(
        hardware.get("board_revision"),
        "hardware.board_revision",
        errors,
        allow_incomplete=allow_incomplete,
    )
    fan_count = hardware.get("fan_count")
    if isinstance(fan_count, bool) or not isinstance(fan_count, int) or not 1 <= fan_count <= 4:
        errors.append("hardware.fan_count must be an integer from 1 through 4")
        fan_count = 0
    fan_index_start = hardware.get("fan_index_start")
    if (
        isinstance(fan_index_start, bool)
        or not isinstance(fan_index_start, int)
        or fan_index_start < 1
    ):
        errors.append("hardware.fan_index_start must be a positive integer")
    dshot_gpios = _gpio_list(hardware.get("dshot_gpios"), "hardware.dshot_gpios", errors)
    if fan_count and len(dshot_gpios) != fan_count:
        errors.append("hardware.dshot_gpios length must equal hardware.fan_count")
    if resolved_config:
        if _config_value(resolved_config, "CONFIG_FAN_COUNT") != fan_count:
            errors.append("hardware.fan_count does not match the release sdkconfig")
        if (
            _config_value(resolved_config, "CONFIG_FAN_INDEX_START")
            != fan_index_start
        ):
            errors.append("hardware.fan_index_start does not match the release sdkconfig")
        if _config_gpio_list(resolved_config, "CONFIG_DSHOT_GPIO") != dshot_gpios:
            errors.append("hardware.dshot_gpios do not match the release sdkconfig")

    esc = _mapping(hardware.get("esc"), "hardware.esc", errors)
    _required_string(
        esc.get("model"), "hardware.esc.model", errors, allow_incomplete=allow_incomplete
    )
    _required_string(
        esc.get("firmware"),
        "hardware.esc.firmware",
        errors,
        allow_incomplete=allow_incomplete,
    )

    interlock = _mapping(hardware.get("interlock"), "hardware.interlock", errors)
    interlock_enabled = interlock.get("enabled")
    if not isinstance(interlock_enabled, bool):
        errors.append("hardware.interlock.enabled must be boolean")
        interlock_enabled = False
    interlock_gpio = interlock.get("gpio")
    if interlock_enabled:
        if (
            isinstance(interlock_gpio, bool)
            or not isinstance(interlock_gpio, int)
            or not 0 <= interlock_gpio <= 48
        ):
            errors.append("hardware.interlock.gpio must be an ESP32-S3 GPIO number (0..48)")
        if not isinstance(interlock.get("active_high"), bool):
            errors.append("hardware.interlock.active_high must be boolean when enabled")
    elif interlock_gpio is not None:
        errors.append("hardware.interlock.gpio must be null when the interlock is disabled")
    if resolved_config:
        configured_interlock = (
            resolved_config.get("CONFIG_MOTOR_INTERLOCK_ENABLED") == "y"
        )
        if configured_interlock != interlock_enabled:
            errors.append("hardware.interlock.enabled does not match the release sdkconfig")
        if interlock_enabled:
            if (
                _config_value(resolved_config, "CONFIG_MOTOR_INTERLOCK_GPIO")
                != interlock_gpio
            ):
                errors.append("hardware.interlock.gpio does not match the release sdkconfig")
            configured_active_high = (
                resolved_config.get("CONFIG_MOTOR_INTERLOCK_ACTIVE_HIGH") == "y"
            )
            if configured_active_high != interlock.get("active_high"):
                errors.append(
                    "hardware.interlock.active_high does not match the release sdkconfig"
                )

    tach = _mapping(hardware.get("tachometer"), "hardware.tachometer", errors)
    tach_enabled = tach.get("enabled")
    if not isinstance(tach_enabled, bool):
        errors.append("hardware.tachometer.enabled must be boolean")
        tach_enabled = False
    tach_gpios = _gpio_list(
        tach.get("gpios"), "hardware.tachometer.gpios", errors
    )
    pulses_per_revolution = tach.get("pulses_per_revolution")
    if tach_enabled:
        if fan_count and len(tach_gpios) != fan_count:
            errors.append("hardware.tachometer.gpios length must equal hardware.fan_count")
        if (
            isinstance(pulses_per_revolution, bool)
            or not isinstance(pulses_per_revolution, int)
            or not 1 <= pulses_per_revolution <= 16
        ):
            errors.append("hardware.tachometer.pulses_per_revolution must be 1..16")
    else:
        if tach_gpios:
            errors.append("hardware.tachometer.gpios must be empty when disabled")
        if pulses_per_revolution is not None:
            errors.append(
                "hardware.tachometer.pulses_per_revolution must be null when disabled"
            )
    if resolved_config:
        configured_tach = resolved_config.get("CONFIG_TACH_FEEDBACK_ENABLED") == "y"
        if configured_tach != tach_enabled:
            errors.append("hardware.tachometer.enabled does not match the release sdkconfig")
        if tach_enabled:
            if _config_gpio_list(resolved_config, "CONFIG_TACH_GPIO") != tach_gpios:
                errors.append("hardware.tachometer.gpios do not match the release sdkconfig")
            if (
                _config_value(resolved_config, "CONFIG_TACH_PULSES_PER_REV")
                != pulses_per_revolution
            ):
                errors.append(
                    "hardware.tachometer.pulses_per_revolution does not match the release sdkconfig"
                )

        configured_node_id = _config_value(resolved_config, "CONFIG_NODE_ID")
        if configured_node_id and configured_node_id != node_id:
            errors.append("release.node_id does not match the release sdkconfig")
        expected_topology = topology_fingerprint_from_sdkconfig(
            resolved_config, node_id
        )
        if expected_topology is not None and topology != expected_topology:
            errors.append("release.topology_fingerprint does not match the release sdkconfig")

    assigned: dict[int, str] = {}
    gpio_groups: list[tuple[str, list[int]]] = [
        ("DShot", dshot_gpios),
        ("tachometer", tach_gpios),
    ]
    if interlock_enabled and isinstance(interlock_gpio, int) and not isinstance(interlock_gpio, bool):
        gpio_groups.append(("interlock", [interlock_gpio]))
    for group, gpios in gpio_groups:
        for gpio in gpios:
            previous = assigned.get(gpio)
            if previous:
                errors.append(f"GPIO {gpio} is assigned to both {previous} and {group}")
            else:
                assigned[gpio] = group

    limits = _mapping(root.get("test_limits"), "test_limits", errors)
    for field in (
        "supply_voltage_v",
        "current_limit_a",
        "maximum_motor_temperature_c",
        "maximum_esc_temperature_c",
        "soak_duration_s",
    ):
        _positive_number(
            limits.get(field),
            f"test_limits.{field}",
            errors,
            allow_incomplete=allow_incomplete,
        )

    artifacts_by_id: dict[str, dict[str, Any]] = {}
    artifact_path_owners: dict[str, str] = {}
    artifacts = _sequence(root.get("artifacts"), "artifacts", errors)
    for index, raw_artifact in enumerate(artifacts):
        label = f"artifacts[{index}]"
        artifact = _mapping(raw_artifact, label, errors)
        artifact_id = artifact.get("id")
        if not isinstance(artifact_id, str) or not IDENTIFIER.fullmatch(artifact_id):
            errors.append(f"{label}.id must be a lowercase identifier")
        elif artifact_id in artifacts_by_id:
            errors.append(f"duplicate artifact id: {artifact_id}")
        else:
            artifacts_by_id[artifact_id] = artifact
        _required_string(
            artifact.get("kind"),
            f"{label}.kind",
            errors,
            allow_incomplete=allow_incomplete,
        )
        relative = _safe_artifact_path(artifact.get("path"), f"{label}.path", errors)
        if relative is not None:
            relative_key = relative.as_posix().casefold()
            previous_owner = artifact_path_owners.get(relative_key)
            if previous_owner is not None:
                errors.append(
                    f"{label}.path duplicates the evidence path used by {previous_owner}"
                )
            else:
                artifact_path_owners[relative_key] = label
        expected = artifact.get("sha256")
        digest_valid = isinstance(expected, str) and HEX_64.fullmatch(expected)
        if not digest_valid:
            errors.append(f"{label}.sha256 must be 64 lowercase hexadecimal characters")
        elif expected == "0" * 64 and not allow_incomplete:
            errors.append(f"{label}.sha256 still contains the template digest")
        if relative is None or allow_incomplete:
            continue
        artifact_path = evidence_root.joinpath(*relative.parts).resolve()
        try:
            artifact_path.relative_to(evidence_root)
        except ValueError:
            errors.append(f"{label}.path resolves outside the evidence directory")
            continue
        if not artifact_path.is_file():
            errors.append(f"{label}.path does not exist: {relative.as_posix()}")
        elif digest_valid and expected != "0" * 64:
            actual = sha256(artifact_path)
            if actual != expected:
                errors.append(f"{label}.sha256 does not match {relative.as_posix()}")

    for artifact_id, expected_kind in REQUIRED_ARTIFACT_KINDS.items():
        artifact = artifacts_by_id.get(artifact_id)
        if artifact is None:
            errors.append(f"required artifact is missing: {artifact_id}")
        elif artifact.get("kind") != expected_kind:
            errors.append(
                f"required artifact {artifact_id} must use kind {expected_kind!r}"
            )

    checks_by_id: dict[str, dict[str, Any]] = {}
    checks = _sequence(root.get("checks"), "checks", errors)
    for index, raw_check in enumerate(checks):
        label = f"checks[{index}]"
        check = _mapping(raw_check, label, errors)
        check_id = check.get("id")
        if not isinstance(check_id, str) or not IDENTIFIER.fullmatch(check_id):
            errors.append(f"{label}.id must be a lowercase identifier")
        elif check_id in checks_by_id:
            errors.append(f"duplicate check id: {check_id}")
        else:
            checks_by_id[check_id] = check
        check_status = check.get("status")
        if check_status not in VALID_STATUSES:
            errors.append(f"{label}.status must be one of {sorted(VALID_STATUSES)}")
        elif not allow_incomplete and check_status in {"fail", "not_run"}:
            errors.append(f"{label}.status={check_status!r} cannot pass a release gate")
        elif check_status == "not_applicable":
            if check_id not in CONDITIONAL_CHECK_IDS:
                errors.append(f"{label} is mandatory and cannot be not_applicable")
            if not isinstance(check.get("notes"), str) or not check["notes"].strip():
                errors.append(f"{label}.notes must justify not_applicable")
        artifact_ids = _sequence(check.get("artifact_ids"), f"{label}.artifact_ids", errors)
        if check_status == "pass" and not artifact_ids:
            errors.append(f"{label} passed without an evidence artifact")
        for artifact_id in artifact_ids:
            if not isinstance(artifact_id, str):
                errors.append(f"{label}.artifact_ids entries must be strings")
            elif artifact_id not in artifacts_by_id:
                errors.append(f"{label} references unknown artifact id: {artifact_id}")
        if not isinstance(check.get("notes"), str):
            errors.append(f"{label}.notes must be a string")

    missing_checks = REQUIRED_CHECK_IDS - checks_by_id.keys()
    for check_id in sorted(missing_checks):
        errors.append(f"required check is missing: {check_id}")
    for check_id, required_artifacts in REQUIRED_CHECK_ARTIFACTS.items():
        check = checks_by_id.get(check_id)
        if check is None:
            continue
        artifact_ids = check.get("artifact_ids")
        referenced = {
            artifact_id
            for artifact_id in artifact_ids
            if isinstance(artifact_id, str)
        } if isinstance(artifact_ids, list) else set()
        for artifact_id in sorted(required_artifacts - referenced):
            errors.append(
                f"required check {check_id} must reference artifact {artifact_id}"
            )
    if interlock_enabled:
        interlock_check = checks_by_id.get("interlock_fail_safe", {})
        if interlock_check.get("status") == "not_applicable":
            errors.append("interlock_fail_safe cannot be not_applicable when interlock is enabled")
    if tach_enabled:
        tach_check = checks_by_id.get("tach_feedback", {})
        if tach_check.get("status") == "not_applicable":
            errors.append("tach_feedback cannot be not_applicable when tachometer is enabled")
    if resolved_config.get("CONFIG_OTA_ENABLED") == "y":
        ota_check = checks_by_id.get("ota_rollback", {})
        if ota_check.get("status") == "not_applicable":
            errors.append("ota_rollback cannot be not_applicable when OTA is enabled")

    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument(
        "--allow-incomplete",
        action="store_true",
        help="validate a draft/template without accepting it as release evidence",
    )
    args = parser.parse_args(argv)
    try:
        manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"HIL evidence validation failed: {exc}", file=sys.stderr)
        return 2

    errors = validate_manifest(
        manifest,
        args.manifest,
        allow_incomplete=args.allow_incomplete,
    )
    if errors:
        print("HIL evidence validation failed:")
        for error in errors:
            print(f"- {error}")
        return 1
    mode = "draft structure" if args.allow_incomplete else "release evidence"
    print(f"HIL {mode} passed ({len(manifest['checks'])} checks, {len(manifest['artifacts'])} artifacts).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
