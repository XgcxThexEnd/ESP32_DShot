import hashlib
import sys
import tempfile
import unittest
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from check_firmware_size import AppPartition, evaluate_image, parse_size, read_app_partitions  # noqa: E402
from check_tracked_generated import blocked_reason  # noqa: E402
from package_firmware_artifacts import redact_sdkconfig, sha256  # noqa: E402


class CiScriptTests(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
