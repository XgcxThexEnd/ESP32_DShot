import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from check_firmware_size import AppPartition, evaluate_image, parse_size, read_app_partitions  # noqa: E402
from check_tracked_generated import blocked_reason  # noqa: E402
from package_firmware_artifacts import (  # noqa: E402
    read_flash_plan,
    read_project_identity,
    redact_sdkconfig,
    resolve_source_commit,
    sha256,
)


class CiScriptTests(unittest.TestCase):
    def test_firmware_job_activates_idf_container_environment(self) -> None:
        workflow = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
        export_command = 'source "$IDF_PATH/export.sh"'
        first_idf_command = 'idf.py -B "$BUILD_DIR"'
        self.assertIn(export_command, workflow)
        self.assertLess(workflow.index(export_command), workflow.index(first_idf_command))

    def test_ci_enforces_hardening_checks_and_feature_matrix(self) -> None:
        workflow = (ROOT / ".github" / "workflows" / "ci.yml").read_text(
            encoding="utf-8"
        )
        self.assertIn("check_git_history_secrets.py", workflow)
        self.assertIn('if [ -z "$base" ]', workflow)
        self.assertIn('python scripts/check_git_history_secrets.py "$head"', workflow)
        self.assertIn("check_production_sdkconfig.py", workflow)
        self.assertIn("validate_hil_evidence.py", workflow)
        for profile in (
            "four-fan-tach-alarm-only",
            "four-fan-tach-stop-fan",
            "four-fan-tach-stop-all",
        ):
            self.assertIn(f"name: {profile}", workflow)
        self.assertIn('--source-commit "$GITHUB_SHA"', workflow)
        self.assertIn("--require-clean-source", workflow)
        self.assertIn("ESP_IDF_BUILDER_IMAGE:", workflow)

    def test_partition_size_suffixes(self) -> None:
        self.assertEqual(parse_size("0x140000"), 0x140000)
        self.assertEqual(parse_size("128K"), 128 * 1024)
        self.assertEqual(parse_size("2m"), 2 * 1024 * 1024)
        with self.assertRaises(ValueError):
            parse_size("-1")

    def test_repository_partition_table_has_ota_layout(self) -> None:
        partitions, has_otadata = read_app_partitions(ROOT / "partitions.csv")
        self.assertTrue(has_otadata)
        self.assertGreaterEqual(len([part for part in partitions if part.subtype.startswith("ota_")]), 2)

    def test_size_gate_reports_both_margin_failures(self) -> None:
        report = evaluate_image(
            900,
            [AppPartition("ota_0", "ota_0", 0, 1000)],
            maximum_utilization=0.85,
            minimum_free_bytes=128,
        )
        self.assertEqual(len(report["failures"]), 2)

    def test_tracked_generated_policy(self) -> None:
        self.assertEqual(blocked_reason(PurePosixPath("sdkconfig")), "generated sdkconfig")
        self.assertEqual(
            blocked_reason(PurePosixPath("build-ci-safe/app.bin")),
            "ESP-IDF build output",
        )
        self.assertIsNone(blocked_reason(PurePosixPath("main/app_main.c")))

    def test_release_sdkconfig_is_redacted_and_hashable(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "sdkconfig"
            destination = Path(directory) / "sdkconfig.redacted"
            source.write_text(
                'CONFIG_WIFI_SSID="secret"\n'
                'CONFIG_MQTT_PASSWORD="secret"\n'
                "CONFIG_FAN_COUNT=4\n",
                encoding="utf-8",
            )
            redact_sdkconfig(source, destination)
            data = destination.read_text(encoding="utf-8")
            self.assertNotIn("secret", data)
            self.assertIn('CONFIG_WIFI_SSID="<redacted>"', data)
            self.assertIn("CONFIG_FAN_COUNT=4", data)
            self.assertEqual(sha256(destination), hashlib.sha256(destination.read_bytes()).hexdigest())

    def test_release_identity_omits_local_build_paths(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            description = Path(directory) / "project_description.json"
            description.write_text(
                json.dumps(
                    {
                        "project_name": "dshot_esc",
                        "project_version": "abc1234",
                        "git_revision": "v6.1",
                        "target": "esp32s3",
                        "project_path": "/private/developer/path",
                        "config_file": "/private/sdkconfig",
                    }
                ),
                encoding="utf-8",
            )
            identity = read_project_identity(description)
            self.assertEqual(identity["project_version"], "abc1234")
            self.assertNotIn("project_path", identity)
            self.assertNotIn("config_file", identity)

    def test_flash_plan_is_flattened_and_rejects_escape(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            build_dir = Path(directory)
            (build_dir / "bootloader").mkdir()
            (build_dir / "bootloader" / "bootloader.bin").write_bytes(b"boot")
            (build_dir / "app.bin").write_bytes(b"app")
            args = {
                "write_flash_args": ["--flash-size", "4MB"],
                "flash_settings": {"flash_size": "4MB"},
                "flash_files": {
                    "0x0": "bootloader/bootloader.bin",
                    "0x10000": "app.bin",
                },
                "extra_esptool_args": {"chip": "esp32s3"},
            }
            (build_dir / "flasher_args.json").write_text(
                json.dumps(args), encoding="utf-8"
            )
            manifest, inputs = read_flash_plan(build_dir)
            self.assertEqual(manifest["flash_files"]["0x0"], "bootloader.bin")
            self.assertEqual(set(inputs), {"bootloader.bin", "app.bin"})

            args["flash_files"]["0x0"] = "../outside.bin"
            (build_dir / "flasher_args.json").write_text(
                json.dumps(args), encoding="utf-8"
            )
            with self.assertRaisesRegex(ValueError, "escapes"):
                read_flash_plan(build_dir)

    def test_source_commit_validation_does_not_accept_labels(self) -> None:
        head = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
        self.assertEqual(resolve_source_commit(head.upper()), head)
        with self.assertRaisesRegex(ValueError, "hexadecimal"):
            resolve_source_commit("main")
        with self.assertRaisesRegex(ValueError, "full hexadecimal"):
            resolve_source_commit("a" * 12)
        different = ("0" if head[0] != "0" else "1") + head[1:]
        with self.assertRaisesRegex(ValueError, "does not match"):
            resolve_source_commit(different)

    def test_flash_plan_rejects_reserved_artifact_names(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            build_dir = Path(directory)
            (build_dir / "FLASH-MANIFEST.json").write_text("{}", encoding="utf-8")
            (build_dir / "flasher_args.json").write_text(
                json.dumps(
                    {"flash_files": {"0x0": "FLASH-MANIFEST.json"}}
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "reserved artifact name"):
                read_flash_plan(build_dir)


if __name__ == "__main__":
    unittest.main()
