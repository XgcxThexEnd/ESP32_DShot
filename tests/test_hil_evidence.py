import copy
import json
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from validate_hil_evidence import (  # noqa: E402
    _read_sdkconfig,
    sha256,
    topology_fingerprint_from_sdkconfig,
    validate_manifest,
)


TEMPLATE = ROOT / "hil" / "evidence-manifest.template.json"


def load_template() -> dict:
    return json.loads(TEMPLATE.read_text(encoding="utf-8"))


def complete_manifest(tmp_path: Path) -> tuple[dict, Path]:
    manifest = copy.deepcopy(load_template())
    manifest_path = tmp_path / "manifest.json"
    manifest["record_status"] = "complete"
    manifest["release"].update(
        {
            "firmware_commit": "a" * 40,
            "tested_at_utc": "2026-09-06T12:00:00Z",
        }
    )
    release = manifest["release"]
    firmware_path = tmp_path / release["firmware_path"]
    firmware_path.parent.mkdir(parents=True, exist_ok=True)
    firmware_path.write_bytes(b"fixture firmware\n")
    release["firmware_sha256"] = sha256(firmware_path)

    sdkconfig_path = tmp_path / release["sdkconfig_redacted_path"]
    sdkconfig_path.write_text(
        "\n".join(
            (
                "CONFIG_FAN_COUNT=4",
                "CONFIG_FAN_INDEX_START=1",
                'CONFIG_NODE_ID="ci-four-fan-safety"',
                'CONFIG_DSHOT_GPIO="5,7,9,11"',
                "CONFIG_MOTOR_INTERLOCK_ENABLED=y",
                "CONFIG_MOTOR_INTERLOCK_GPIO=4",
                "CONFIG_MOTOR_INTERLOCK_ACTIVE_HIGH=y",
                "CONFIG_TACH_FEEDBACK_ENABLED=y",
                'CONFIG_TACH_GPIO="6,8,10,12"',
                "CONFIG_TACH_PULSES_PER_REV=2",
                "# CONFIG_OTA_ENABLED is not set",
                "",
            )
        ),
        encoding="utf-8",
    )
    release["sdkconfig_redacted_sha256"] = sha256(sdkconfig_path)
    config_errors: list[str] = []
    config = _read_sdkconfig(sdkconfig_path, config_errors)
    assert config_errors == []
    release["topology_fingerprint"] = topology_fingerprint_from_sdkconfig(
        config, release["node_id"]
    )

    flash_path = tmp_path / release["flash_manifest_path"]
    bootloader_path = flash_path.parent / "bootloader.bin"
    partition_path = flash_path.parent / "partition-table.bin"
    ota_data_path = flash_path.parent / "ota_data_initial.bin"
    bootloader_path.write_bytes(b"fixture bootloader\n")
    partition_path.write_bytes(b"fixture partition table\n")
    ota_data_path.write_bytes(b"fixture OTA selection data\n")
    flash_path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "flash_files": {
                    "0x0": bootloader_path.name,
                    "0x8000": partition_path.name,
                    "0x10000": firmware_path.name,
                    "0x3d0000": ota_data_path.name,
                },
                "files": [
                    {
                        "offset": "0x0",
                        "file": bootloader_path.name,
                        "sha256": sha256(bootloader_path),
                    },
                    {
                        "offset": "0x8000",
                        "file": partition_path.name,
                        "sha256": sha256(partition_path),
                    },
                    {
                        "offset": "0x10000",
                        "file": firmware_path.name,
                        "sha256": release["firmware_sha256"],
                    },
                    {
                        "offset": "0x3d0000",
                        "file": ota_data_path.name,
                        "sha256": sha256(ota_data_path),
                    },
                ],
            }
        ),
        encoding="utf-8",
    )
    release["flash_manifest_sha256"] = sha256(flash_path)

    metadata_path = tmp_path / release["release_metadata_path"]
    metadata_path.write_text(
        json.dumps(
            {
                "schema_version": 2,
                "source_commit": release["firmware_commit"],
                "configuration": release["configuration"],
                "redacted_configuration_sha256": release[
                    "sdkconfig_redacted_sha256"
                ],
                "flash_manifest": flash_path.name,
                "source_tree_dirty": False,
                "build_version_matches_source": True,
                "build": {"git_revision": release["esp_idf_version"]},
            }
        ),
        encoding="utf-8",
    )
    release["release_metadata_sha256"] = sha256(metadata_path)
    manifest["hardware"]["board_revision"] = "fixture-rev-a"
    manifest["hardware"]["esc"] = {
        "model": "fixture-esc",
        "firmware": "fixture-fw-1",
    }
    manifest["test_limits"] = {
        "supply_voltage_v": 24.0,
        "current_limit_a": 2.0,
        "maximum_motor_temperature_c": 70.0,
        "maximum_esc_temperature_c": 80.0,
        "soak_duration_s": 3600,
    }
    for artifact in manifest["artifacts"]:
        path = tmp_path / Path(artifact["path"])
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f"fixture for {artifact['id']}\n", encoding="utf-8")
        artifact["sha256"] = sha256(path)
    for check in manifest["checks"]:
        check["status"] = "pass"
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    return manifest, manifest_path


def test_repository_template_is_a_valid_incomplete_record() -> None:
    manifest = load_template()

    assert validate_manifest(manifest, TEMPLATE, allow_incomplete=True) == []
    strict_errors = validate_manifest(manifest, TEMPLATE)
    assert any("record_status" in error for error in strict_errors)
    assert any("template digest" in error for error in strict_errors)
    assert any("cannot pass a release gate" in error for error in strict_errors)


def test_complete_manifest_with_matching_artifacts_passes(tmp_path: Path) -> None:
    manifest, manifest_path = complete_manifest(tmp_path)

    assert validate_manifest(manifest, manifest_path) == []


def test_changed_artifact_and_unsafe_path_are_rejected(tmp_path: Path) -> None:
    manifest, manifest_path = complete_manifest(tmp_path)
    first = manifest["artifacts"][0]
    (tmp_path / first["path"]).write_text("tampered\n", encoding="utf-8")

    errors = validate_manifest(manifest, manifest_path)
    assert any("sha256 does not match" in error for error in errors)

    first["path"] = "../outside.csv"
    errors = validate_manifest(manifest, manifest_path, allow_incomplete=True)
    assert any("must remain inside" in error for error in errors)


def test_release_identity_files_are_required_and_cross_checked(tmp_path: Path) -> None:
    manifest, manifest_path = complete_manifest(tmp_path)
    metadata_path = tmp_path / manifest["release"]["release_metadata_path"]
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    metadata["source_commit"] = "c" * 40
    metadata_path.write_text(json.dumps(metadata), encoding="utf-8")
    manifest["release"]["release_metadata_sha256"] = sha256(metadata_path)

    errors = validate_manifest(manifest, manifest_path)

    assert any("source_commit does not match" in error for error in errors)

    firmware_path = tmp_path / manifest["release"]["firmware_path"]
    firmware_path.unlink()
    errors = validate_manifest(manifest, manifest_path)
    assert any("firmware_path does not exist" in error for error in errors)


def test_empty_or_sensitive_release_sdkconfig_is_rejected(tmp_path: Path) -> None:
    manifest, manifest_path = complete_manifest(tmp_path)
    release = manifest["release"]
    sdkconfig_path = tmp_path / release["sdkconfig_redacted_path"]
    metadata_path = tmp_path / release["release_metadata_path"]
    valid_sdkconfig = sdkconfig_path.read_text(encoding="utf-8")

    sdkconfig_path.write_text("# no resolved configuration\n", encoding="utf-8")
    release["sdkconfig_redacted_sha256"] = sha256(sdkconfig_path)
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    metadata["redacted_configuration_sha256"] = release[
        "sdkconfig_redacted_sha256"
    ]
    metadata_path.write_text(json.dumps(metadata), encoding="utf-8")
    release["release_metadata_sha256"] = sha256(metadata_path)

    errors = validate_manifest(manifest, manifest_path)
    assert any("contains no parseable sdkconfig entries" in error for error in errors)

    sdkconfig_path.write_text(
        valid_sdkconfig + 'CONFIG_WIFI_PASS="do-not-disclose"\n',
        encoding="utf-8",
    )
    release["sdkconfig_redacted_sha256"] = sha256(sdkconfig_path)
    metadata["redacted_configuration_sha256"] = release[
        "sdkconfig_redacted_sha256"
    ]
    metadata_path.write_text(json.dumps(metadata), encoding="utf-8")
    release["release_metadata_sha256"] = sha256(metadata_path)

    errors = validate_manifest(manifest, manifest_path)
    assert any("populated sensitive value: CONFIG_WIFI_PASS" in error for error in errors)
    assert all("do-not-disclose" not in error for error in errors)


def test_flash_manifest_schema_plan_and_every_file_are_bound(tmp_path: Path) -> None:
    manifest, manifest_path = complete_manifest(tmp_path)
    release = manifest["release"]
    flash_path = tmp_path / release["flash_manifest_path"]
    flash = json.loads(flash_path.read_text(encoding="utf-8"))
    flash["schema_version"] = 999
    flash_path.write_text(json.dumps(flash), encoding="utf-8")
    release["flash_manifest_sha256"] = sha256(flash_path)

    errors = validate_manifest(manifest, manifest_path)
    assert any("must use schema_version 1" in error for error in errors)

    flash["schema_version"] = 1
    flash["flash_files"]["0x10000"] = "different.bin"
    flash_path.write_text(json.dumps(flash), encoding="utf-8")
    release["flash_manifest_sha256"] = sha256(flash_path)

    errors = validate_manifest(manifest, manifest_path)
    assert any("does not match flash_files" in error for error in errors)

    flash["flash_files"].pop("0x8000")
    flash["files"] = [
        entry for entry in flash["files"] if entry["offset"] != "0x8000"
    ]
    flash_path.write_text(json.dumps(flash), encoding="utf-8")
    release["flash_manifest_sha256"] = sha256(flash_path)

    errors = validate_manifest(manifest, manifest_path)
    assert any(
        "missing required file partition-table.bin at 0x8000" in error
        for error in errors
    )

    manifest, manifest_path = complete_manifest(tmp_path / "corrupt")
    release = manifest["release"]
    bootloader_path = (
        (tmp_path / "corrupt") / release["flash_manifest_path"]
    ).parent / "bootloader.bin"
    bootloader_path.write_bytes(b"tampered bootloader\n")

    errors = validate_manifest(manifest, manifest_path)
    assert any("sha256 does not match bootloader.bin" in error for error in errors)


def test_decimal_flash_offset_is_validated_without_crashing(tmp_path: Path) -> None:
    manifest, manifest_path = complete_manifest(tmp_path)
    release = manifest["release"]
    flash_path = tmp_path / release["flash_manifest_path"]
    flash = json.loads(flash_path.read_text(encoding="utf-8"))
    flash["flash_files"]["32768"] = flash["flash_files"].pop("0x8000")
    partition_entry = next(
        entry for entry in flash["files"] if entry["offset"] == "0x8000"
    )
    partition_entry["offset"] = "32768"
    flash_path.write_text(json.dumps(flash), encoding="utf-8")
    release["flash_manifest_sha256"] = sha256(flash_path)

    assert validate_manifest(manifest, manifest_path) == []


def test_required_artifact_contract_cannot_be_replaced_by_one_file(
    tmp_path: Path,
) -> None:
    manifest, manifest_path = complete_manifest(tmp_path)
    one_path = tmp_path / "artifacts" / "one.txt"
    one_path.write_text("not all of the required evidence\n", encoding="utf-8")
    manifest["artifacts"] = [
        {
            "id": "one",
            "kind": "anything",
            "path": "artifacts/one.txt",
            "sha256": sha256(one_path),
        }
    ]
    for check in manifest["checks"]:
        check["artifact_ids"] = ["one"]

    errors = validate_manifest(manifest, manifest_path)
    assert any("required artifact is missing: boot-dshot-capture" in error for error in errors)
    assert any(
        "required check dshot_timing must reference artifact steady-dshot-capture"
        in error
        for error in errors
    )


def test_required_artifact_kinds_paths_and_check_mappings_are_fixed(
    tmp_path: Path,
) -> None:
    manifest, manifest_path = complete_manifest(tmp_path)
    artifacts = {artifact["id"]: artifact for artifact in manifest["artifacts"]}
    artifacts["boot-dshot-capture"]["kind"] = "unstructured_text"
    artifacts["steady-dshot-capture"]["path"] = artifacts[
        "boot-dshot-capture"
    ]["path"]
    artifacts["steady-dshot-capture"]["sha256"] = artifacts[
        "boot-dshot-capture"
    ]["sha256"]
    timing = next(
        check for check in manifest["checks"] if check["id"] == "dshot_timing"
    )
    timing["artifact_ids"] = ["boot-dshot-capture"]

    errors = validate_manifest(manifest, manifest_path)
    assert any(
        "boot-dshot-capture must use kind 'logic_analyzer_csv'" in error
        for error in errors
    )
    assert any("duplicates the evidence path" in error for error in errors)
    assert any(
        "dshot_timing must reference artifact steady-dshot-capture" in error
        for error in errors
    )


def test_sha256_git_object_id_is_accepted(tmp_path: Path) -> None:
    manifest, manifest_path = complete_manifest(tmp_path)
    release = manifest["release"]
    release["firmware_commit"] = "a" * 64
    metadata_path = tmp_path / release["release_metadata_path"]
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    metadata["source_commit"] = release["firmware_commit"]
    metadata_path.write_text(json.dumps(metadata), encoding="utf-8")
    release["release_metadata_sha256"] = sha256(metadata_path)

    assert validate_manifest(manifest, manifest_path) == []


def test_gpio_collisions_and_mandatory_not_applicable_are_rejected() -> None:
    manifest = load_template()
    manifest["hardware"]["tachometer"]["gpios"][0] = 5
    timing = next(check for check in manifest["checks"] if check["id"] == "dshot_timing")
    timing["status"] = "not_applicable"
    timing["notes"] = "Attempted waiver"

    errors = validate_manifest(manifest, TEMPLATE, allow_incomplete=True)

    assert any("assigned to both DShot and tachometer" in error for error in errors)
    assert any("mandatory and cannot be not_applicable" in error for error in errors)


def test_compile_profiles_cover_four_fans_and_every_tach_action() -> None:
    common = (ROOT / "configs" / "sdkconfig.ci-four-fan-safety.defaults").read_text(
        encoding="utf-8"
    )
    assert "CONFIG_FAN_COUNT=4" in common
    assert 'CONFIG_DSHOT_GPIO="5,7,9,11"' in common
    assert "CONFIG_MOTOR_INTERLOCK_ENABLED=y" in common
    assert "CONFIG_MOTOR_INTERLOCK_GPIO=4" in common
    assert "CONFIG_TACH_FEEDBACK_ENABLED=y" in common
    assert 'CONFIG_TACH_GPIO="6,8,10,12"' in common

    profile_names = {
        "sdkconfig.ci-tach-alarm-only.defaults": "CONFIG_TACH_STALL_ALARM_ONLY=y",
        "sdkconfig.ci-tach-stop-fan.defaults": "CONFIG_TACH_STALL_STOP_FAN=y",
        "sdkconfig.ci-tach-stop-all.defaults": "CONFIG_TACH_STALL_STOP_ALL=y",
    }
    for filename, selected in profile_names.items():
        profile = (ROOT / "configs" / filename).read_text(encoding="utf-8")
        assert profile.count("=y") == 1
        assert selected in profile
        assert "CONFIG_WIFI_" not in profile
        assert "CONFIG_MQTT_" not in profile
