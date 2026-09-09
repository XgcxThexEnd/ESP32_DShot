import json
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from check_git_history_secrets import scan_blob, scan_repository  # noqa: E402
from check_production_sdkconfig import (  # noqa: E402
    BASELINE_EXPECTED,
    SECRET_SYMBOLS,
    TEMPLATE_EXPECTED,
    _read_json_policy,
    parse_sdkconfig,
    validate_sdkconfig,
)
from check_repository_hygiene import is_blocked_filename, is_private_path  # noqa: E402


def sdkconfig_text(values: dict[str, object]) -> str:
    lines = []
    for symbol, value in values.items():
        if value == "n":
            lines.append(f"# {symbol} is not set")
        elif value == "y":
            lines.append(f"{symbol}=y")
        elif isinstance(value, str):
            lines.append(f"{symbol}={json.dumps(value)}")
        else:
            lines.append(f"{symbol}={value}")
    return "\n".join(lines) + "\n"


def test_production_template_policy_accepts_conservative_ci_profile() -> None:
    values: dict[str, object] = {**BASELINE_EXPECTED, **TEMPLATE_EXPECTED}
    values.update({symbol: "" for symbol in SECRET_SYMBOLS})
    parsed = parse_sdkconfig(sdkconfig_text(values))

    assert validate_sdkconfig(parsed, mode="template") == []


def test_identical_sdkconfig_aliases_are_allowed_but_conflicts_are_not() -> None:
    identical = parse_sdkconfig("CONFIG_EXAMPLE=y\nCONFIG_EXAMPLE=y\n")
    conflict = parse_sdkconfig(
        "CONFIG_EXAMPLE=y\n# CONFIG_EXAMPLE is not set\n"
    )

    assert identical.duplicates == frozenset()
    assert conflict.duplicates == frozenset({"CONFIG_EXAMPLE"})


def test_example_release_policy_is_complete_and_matches_reviewed_profile() -> None:
    policy = _read_json_policy(
        ROOT / "configs" / "production-release-policy.example.json"
    )
    values: dict[str, object] = {
        **BASELINE_EXPECTED,
        **policy.decisions,
        **policy.required_values,
        "CONFIG_WIFI_SSID": "device-network",
        "CONFIG_WIFI_PASS": "not-a-real-password",
        "CONFIG_MQTT_BROKER_URI": "mqtts://broker.example.invalid",
        "CONFIG_MQTT_USERNAME": "device-identity",
        "CONFIG_MQTT_PASSWORD": "not-a-real-password",
    }
    parsed = parse_sdkconfig(sdkconfig_text(values))

    assert validate_sdkconfig(parsed, mode="release", policy=policy) == []


def test_release_policy_diagnostics_do_not_disclose_values() -> None:
    policy = _read_json_policy(
        ROOT / "configs" / "production-release-policy.example.json"
    )
    values: dict[str, object] = {
        **BASELINE_EXPECTED,
        **policy.decisions,
        **policy.required_values,
        "CONFIG_WIFI_SSID": "highly-sensitive-ssid",
        "CONFIG_WIFI_PASS": "highly-sensitive-wifi-password",
        "CONFIG_MQTT_BROKER_URI": "mqtt://private-broker-name",
        "CONFIG_MQTT_USERNAME": "highly-sensitive-user",
        "CONFIG_MQTT_PASSWORD": "highly-sensitive-mqtt-password",
    }
    findings = validate_sdkconfig(
        parse_sdkconfig(sdkconfig_text(values)), mode="release", policy=policy
    )
    output = "\n".join(finding.display() for finding in findings)

    assert "CONFIG_MQTT_BROKER_URI" in output
    assert "private-broker-name" not in output
    assert "highly-sensitive" not in output


def test_release_policy_rejects_broker_userinfo() -> None:
    policy = _read_json_policy(
        ROOT / "configs" / "production-release-policy.example.json"
    )
    values: dict[str, object] = {
        **BASELINE_EXPECTED,
        **policy.decisions,
        **policy.required_values,
        "CONFIG_WIFI_SSID": "device-network",
        "CONFIG_WIFI_PASS": "not-a-real-password",
        "CONFIG_MQTT_BROKER_URI": (
            "mqtts://" + "embedded" + ":" + "credential" + "@broker.example.invalid"
        ),
        "CONFIG_MQTT_USERNAME": "device-identity",
        "CONFIG_MQTT_PASSWORD": "not-a-real-password",
    }

    findings = validate_sdkconfig(
        parse_sdkconfig(sdkconfig_text(values)), mode="release", policy=policy
    )

    assert any(
        finding.symbol == "CONFIG_MQTT_BROKER_URI"
        and "no userinfo" in finding.reason
        for finding in findings
    )


def test_release_policy_rejects_development_encryption_mode() -> None:
    policy = _read_json_policy(
        ROOT / "configs" / "production-release-policy.example.json"
    )
    values: dict[str, object] = {
        **BASELINE_EXPECTED,
        **policy.decisions,
        **policy.required_values,
        "CONFIG_WIFI_SSID": "device-network",
        "CONFIG_WIFI_PASS": "not-a-real-password",
        "CONFIG_MQTT_BROKER_URI": "mqtts://broker.example.invalid",
        "CONFIG_MQTT_USERNAME": "device-identity",
        "CONFIG_MQTT_PASSWORD": "not-a-real-password",
        "CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT": "y",
        "CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE": "n",
    }

    findings = validate_sdkconfig(
        parse_sdkconfig(sdkconfig_text(values)), mode="release", policy=policy
    )

    assert any("must use release mode" in finding.reason for finding in findings)


def test_signed_apps_without_secure_boot_must_verify_updates() -> None:
    policy = _read_json_policy(
        ROOT / "configs" / "production-release-policy.example.json"
    )
    policy.decisions.update(
        {
            "CONFIG_SECURE_BOOT": "n",
            "CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT": "y",
            "CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT": "n",
        }
    )
    values: dict[str, object] = {
        **BASELINE_EXPECTED,
        **policy.decisions,
        **policy.required_values,
        "CONFIG_WIFI_SSID": "device-network",
        "CONFIG_WIFI_PASS": "not-a-real-password",
        "CONFIG_MQTT_BROKER_URI": "mqtts://broker.example.invalid",
        "CONFIG_MQTT_USERNAME": "device-identity",
        "CONFIG_MQTT_PASSWORD": "not-a-real-password",
    }

    findings = validate_sdkconfig(
        parse_sdkconfig(sdkconfig_text(values)), mode="release", policy=policy
    )

    assert any("does not verify OTA updates" in finding.reason for finding in findings)


def test_history_blob_scan_reports_markers_without_values() -> None:
    data = (
        b'CONFIG_MQTT_PASSWORD="do-not-print-this"\n'
        b"-----BEGIN " + b"PRIVATE KEY-----\n"
        b"mqtt://" + b"actual-user" + b":" + b"actual-password" + b"@internal-broker\n"
    )
    findings = scan_blob(data, "sdkconfig", "0123456789ab")
    output = "\n".join(finding.display() for finding in findings)

    assert "CONFIG_MQTT_PASSWORD" in output
    assert "private-key-marker" in output
    assert "embedded-mqtt-credentials" in output
    assert "do-not-print-this" not in output
    assert "actual-user" not in output
    assert "actual-password" not in output


def test_history_blob_scan_ignores_documented_placeholder_uri() -> None:
    findings = scan_blob(
        b'example = "mqtt://user:pass@broker"\n',
        "tests/example.py",
        "0123456789ab",
    )
    assert findings == []


def test_history_blob_scan_blocks_local_env_but_allows_example() -> None:
    local_findings = scan_blob(b"SAFE=value\n", ".env.production", "0123456789ab")
    example_findings = scan_blob(b"SAFE=value\n", ".env.example", "0123456789ab")

    assert any(finding.marker == "sensitive-file-name" for finding in local_findings)
    assert example_findings == []


def test_worktree_hygiene_blocks_local_env_but_allows_example() -> None:
    assert is_blocked_filename(".env.production")
    assert is_blocked_filename(".ENV.local")
    assert is_blocked_filename("device.private.pem")
    assert is_blocked_filename("signer.signing.pem")
    assert is_blocked_filename("factory.nvs.csv")
    assert not is_blocked_filename(".env.example")
    assert is_private_path("Secrets/device.txt")
    assert is_private_path("Provisioning/Private/device.txt")
    assert not is_private_path("docs/provisioning.md")


def test_history_range_detects_existing_blob_copied_to_sensitive_path(
    tmp_path: Path,
) -> None:
    repo = tmp_path / "repository"
    repo.mkdir()

    def git(*arguments: str) -> None:
        subprocess.run(
            ["git", *arguments],
            cwd=repo,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    git("init", "--quiet")
    git("config", "user.name", "History Scanner Test")
    git("config", "user.email", "history-scanner@example.invalid")
    source = repo / "ordinary-config"
    source.write_text("SAFE=value\n", encoding="utf-8")
    git("add", "ordinary-config")
    git("commit", "--quiet", "-m", "base")

    (repo / ".env.production").write_bytes(source.read_bytes())
    git("add", ".env.production")
    git("commit", "--quiet", "-m", "copy existing blob to sensitive path")

    findings = scan_repository(repo, ["HEAD^..HEAD"])

    assert any(
        finding.path == ".env.production"
        and finding.marker == "sensitive-file-name"
        for finding in findings
    )
