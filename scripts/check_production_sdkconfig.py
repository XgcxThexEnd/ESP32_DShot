#!/usr/bin/env python3
"""Validate a resolved ESP-IDF production configuration without printing values.

Two modes are intentionally separate:

* ``template`` validates the non-secret configuration built by ordinary CI.
  Credential fields must be empty and irreversible/device-specific choices use
  the repository's conservative template values.
* ``release`` validates a deployable resolved sdkconfig.  Credentials must be
  populated, and a reviewed JSON policy must record every security/safety
  decision whose correct value depends on the device class.

Diagnostics contain only CONFIG symbol names and reason categories.  Actual and
expected values are never included in output.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit


ASSIGNMENT = re.compile(r"^(CONFIG_[A-Z0-9_]+)=(.*)$")
NOT_SET = re.compile(r"^# (CONFIG_[A-Z0-9_]+) is not set$")
CONFIG_NAME = re.compile(r"^CONFIG_[A-Z0-9_]+$")
SENSITIVE_NAME = re.compile(
    r"(?:^|_)(?:PASSWORD|PASS|PASSPHRASE|SSID|USERNAME|SECRET|TOKEN)(?:_|$)|"
    r"(?:^|_)(?:BROKER_URI|PRIVATE_KEY|SIGNING_KEY|CLIENT_KEY)(?:_|$)"
)


BASELINE_EXPECTED = {
    "CONFIG_WIFI_REQUIRE_SECURE_AUTH": "y",
    "CONFIG_WIFI_PMF_REQUIRED": "y",
    "CONFIG_MQTT_REQUIRE_TLS_AUTH": "y",
    "CONFIG_MQTT_TRANSPORT_SSL": "y",
    "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE": "y",
    "CONFIG_MBEDTLS_HAVE_TIME_DATE": "y",
    "CONFIG_SUPERVISOR_ENABLED": "y",
    "CONFIG_RMT_FAILURE_REBOOT": "y",
    "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE": "y",
    "CONFIG_MQTT_LEGACY_TOPICS": "n",
    "CONFIG_ESP_TLS_INSECURE": "n",
    "CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP": "n",
}

SECRET_SYMBOLS = (
    "CONFIG_WIFI_SSID",
    "CONFIG_WIFI_PASS",
    "CONFIG_MQTT_BROKER_URI",
    "CONFIG_MQTT_USERNAME",
    "CONFIG_MQTT_PASSWORD",
)

# A release policy must state these values even when the selected value is n.
# That makes a risk-sensitive choice reviewable instead of silently inheriting
# whichever default a future ESP-IDF/Kconfig revision happens to provide.
RELEASE_DECISION_SYMBOLS = frozenset(
    {
        "CONFIG_MOTOR_INTERLOCK_ENABLED",
        "CONFIG_TACH_FEEDBACK_ENABLED",
        "CONFIG_COMM_LOSS_CONTINUE",
        "CONFIG_COMM_LOSS_STOP_AFTER_LEASE",
        "CONFIG_RESTORE_FAN_STATE",
        "CONFIG_SCHEDULE_ENABLED",
        "CONFIG_OTA_ENABLED",
        "CONFIG_MQTT_RESTART_ON_TIMEOUT",
        "CONFIG_HOME_ASSISTANT_DISCOVERY_ENABLED",
        "CONFIG_MQTT_LEGACY_TOPICS",
        "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE",
        "CONFIG_SECURE_BOOT",
        "CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT",
        "CONFIG_SECURE_FLASH_ENC_ENABLED",
        "CONFIG_NVS_ENCRYPTION",
        "CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK",
    }
)

RELEASE_REVIEWED_VALUE_SYMBOLS = frozenset(
    {
        "CONFIG_FAN_COUNT",
        "CONFIG_FAN_INDEX_START",
        "CONFIG_DSHOT_GPIO",
        "CONFIG_MIN_SPIN_PCT",
        "CONFIG_MQTT_ROOT_TOPIC",
    }
)

TEMPLATE_EXPECTED = {
    "CONFIG_MOTOR_INTERLOCK_ENABLED": "n",
    "CONFIG_TACH_FEEDBACK_ENABLED": "n",
    "CONFIG_COMM_LOSS_CONTINUE": "y",
    "CONFIG_COMM_LOSS_STOP_AFTER_LEASE": "n",
    "CONFIG_RESTORE_FAN_STATE": "n",
    "CONFIG_SCHEDULE_ENABLED": "n",
    "CONFIG_OTA_ENABLED": "n",
    "CONFIG_MQTT_RESTART_ON_TIMEOUT": "n",
    "CONFIG_HOME_ASSISTANT_DISCOVERY_ENABLED": "n",
    "CONFIG_SECURE_BOOT": "n",
    "CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT": "n",
    "CONFIG_SECURE_FLASH_ENC_ENABLED": "n",
    "CONFIG_NVS_ENCRYPTION": "n",
    "CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK": "n",
}


@dataclass(frozen=True, order=True)
class Finding:
    symbol: str
    reason: str

    def display(self) -> str:
        return f"{self.symbol}: {self.reason}"


@dataclass(frozen=True)
class ParsedSdkconfig:
    values: dict[str, str]
    duplicates: frozenset[str]


@dataclass(frozen=True)
class ReleasePolicy:
    decisions: dict[str, str]
    required_values: dict[str, Any]


class PolicyError(ValueError):
    """A policy is malformed or incomplete."""


def parse_sdkconfig(text: str) -> ParsedSdkconfig:
    values: dict[str, str] = {}
    duplicates: set[str] = set()
    for line in text.splitlines():
        assignment = ASSIGNMENT.fullmatch(line)
        unset = NOT_SET.fullmatch(line)
        if assignment:
            symbol, value = assignment.groups()
        elif unset:
            symbol, value = unset.group(1), "n"
        else:
            continue
        # ESP-IDF can emit identical compatibility aliases more than once in a
        # resolved sdkconfig. Only contradictory definitions are ambiguous.
        if symbol in values and values[symbol] != value:
            duplicates.add(symbol)
        values[symbol] = value
    return ParsedSdkconfig(values, frozenset(duplicates))


def _decoded_value(raw: str) -> Any:
    if len(raw) >= 2 and raw[0] == '"' and raw[-1] == '"':
        try:
            return json.loads(raw)
        except json.JSONDecodeError:
            return raw
    try:
        return int(raw, 0)
    except ValueError:
        return raw


def _is_empty(raw: str) -> bool:
    decoded = _decoded_value(raw)
    return decoded == ""


def _read_json_policy(path: Path) -> ReleasePolicy:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise PolicyError(f"could not read policy ({exc.__class__.__name__})") from exc
    except json.JSONDecodeError as exc:
        raise PolicyError(
            f"policy is not valid JSON (line {exc.lineno}, column {exc.colno})"
        ) from exc

    if not isinstance(document, dict):
        raise PolicyError("policy root must be a JSON object")
    allowed_keys = {"schema_version", "decisions", "required_values"}
    unknown_keys = set(document) - allowed_keys
    if unknown_keys:
        raise PolicyError("policy contains unsupported top-level fields")
    if document.get("schema_version") != 1:
        raise PolicyError("policy schema_version must be 1")

    decisions = document.get("decisions")
    required_values = document.get("required_values")
    if not isinstance(decisions, dict) or not isinstance(required_values, dict):
        raise PolicyError("policy decisions and required_values must be JSON objects")

    normalized_decisions: dict[str, str] = {}
    for symbol, expected in decisions.items():
        if not isinstance(symbol, str) or not CONFIG_NAME.fullmatch(symbol):
            raise PolicyError("policy contains an invalid CONFIG symbol name")
        if expected not in {"y", "n"}:
            raise PolicyError("policy decision values must be y or n")
        normalized_decisions[symbol] = expected

    missing_decisions = RELEASE_DECISION_SYMBOLS - normalized_decisions.keys()
    if missing_decisions:
        raise PolicyError("policy does not record every required release decision")

    normalized_values: dict[str, Any] = {}
    for symbol, expected in required_values.items():
        if not isinstance(symbol, str) or not CONFIG_NAME.fullmatch(symbol):
            raise PolicyError("policy contains an invalid CONFIG symbol name")
        if SENSITIVE_NAME.search(symbol):
            raise PolicyError("policy required_values must never contain secret symbols")
        if isinstance(expected, bool) or not isinstance(expected, (str, int)):
            raise PolicyError("policy required_values must be strings or integers")
        normalized_values[symbol] = expected

    missing_values = RELEASE_REVIEWED_VALUE_SYMBOLS - normalized_values.keys()
    if missing_values:
        raise PolicyError("policy omits required device/topology values")

    return ReleasePolicy(normalized_decisions, normalized_values)


def _expect(
    parsed: ParsedSdkconfig,
    expected: dict[str, str],
    findings: list[Finding],
    reason: str,
) -> None:
    for symbol, required in expected.items():
        actual = parsed.values.get(symbol)
        if actual is None:
            findings.append(Finding(symbol, "required symbol is absent"))
        elif actual != required:
            findings.append(Finding(symbol, reason))


def _validate_secret_fields(
    parsed: ParsedSdkconfig, mode: str, findings: list[Finding]
) -> None:
    for symbol in SECRET_SYMBOLS:
        raw = parsed.values.get(symbol)
        if raw is None:
            findings.append(Finding(symbol, "credential symbol is absent"))
            continue
        empty = _is_empty(raw)
        if mode == "template" and not empty:
            findings.append(Finding(symbol, "CI template must not contain a value"))
        elif mode == "release" and empty:
            findings.append(Finding(symbol, "release credential is empty"))

    broker_raw = parsed.values.get("CONFIG_MQTT_BROKER_URI")
    if mode == "release" and broker_raw is not None and not _is_empty(broker_raw):
        broker = _decoded_value(broker_raw)
        if not _valid_secure_uri(broker, scheme="mqtts", require_trailing_slash=False):
            findings.append(
                Finding(
                    "CONFIG_MQTT_BROKER_URI",
                    "release broker URI must be mqtts with a host and no userinfo",
                )
            )


def _valid_secure_uri(
    value: Any, *, scheme: str, require_trailing_slash: bool
) -> bool:
    if not isinstance(value, str) or any(
        char.isspace() or ord(char) == 0x7F or char in "\\%" for char in value
    ):
        return False
    try:
        parsed = urlsplit(value)
        # Accessing port also rejects malformed/non-numeric ports.
        _ = parsed.port
    except ValueError:
        return False
    if (
        parsed.scheme != scheme
        or not parsed.hostname
        or parsed.username is not None
        or parsed.password is not None
        or parsed.fragment
    ):
        return False
    if scheme == "mqtts" and (parsed.path not in {"", "/"} or parsed.query):
        return False
    if require_trailing_slash and (not parsed.path.endswith("/") or parsed.query):
        return False
    if any(segment in {".", ".."} for segment in parsed.path.split("/")):
        return False
    return True


def _validate_choice(
    parsed: ParsedSdkconfig, symbols: tuple[str, ...], findings: list[Finding]
) -> None:
    selected = [symbol for symbol in symbols if parsed.values.get(symbol) == "y"]
    if len(selected) != 1:
        findings.append(Finding(symbols[0], "choice must select exactly one option"))


def _require_policy_values(
    policy: ReleasePolicy,
    symbols: set[str],
    findings: list[Finding],
) -> None:
    for symbol in sorted(symbols):
        if symbol not in policy.required_values:
            findings.append(Finding(symbol, "release policy must review this value"))


def _validate_release_conditionals(
    parsed: ParsedSdkconfig,
    policy: ReleasePolicy,
    findings: list[Finding],
) -> None:
    secure_boot = parsed.values.get("CONFIG_SECURE_BOOT") == "y"
    signed_without_secure_boot = (
        parsed.values.get("CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT") == "y"
    )
    if not secure_boot and not signed_without_secure_boot:
        findings.append(
            Finding(
                "CONFIG_SECURE_BOOT",
                "release image verification is not enabled",
            )
        )
    if parsed.values.get("CONFIG_SECURE_BOOT_INSECURE") == "y":
        findings.append(
            Finding(
                "CONFIG_SECURE_BOOT_INSECURE",
                "potentially insecure secure-boot options are enabled",
            )
        )
    if signed_without_secure_boot and (
        parsed.values.get("CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT") != "y"
    ):
        findings.append(
            Finding(
                "CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT",
                "signed-app mode does not verify OTA updates",
            )
        )

    if parsed.values.get("CONFIG_SECURE_FLASH_ENC_ENABLED") == "y":
        encryption_modes = (
            "CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT",
            "CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE",
        )
        for symbol in encryption_modes:
            if symbol not in policy.decisions:
                findings.append(
                    Finding(symbol, "release policy must record flash-encryption mode")
                )
        _validate_choice(parsed, encryption_modes, findings)
        if parsed.values.get("CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE") != "y":
            findings.append(
                Finding(
                    "CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE",
                    "production flash encryption must use release mode",
                )
            )

    if parsed.values.get("CONFIG_OTA_ENABLED") == "y":
        _require_policy_values(policy, {"CONFIG_OTA_ALLOWED_URL_PREFIX"}, findings)
        prefix_raw = parsed.values.get("CONFIG_OTA_ALLOWED_URL_PREFIX")
        prefix = _decoded_value(prefix_raw) if prefix_raw is not None else None
        if not _valid_secure_uri(
            prefix, scheme="https", require_trailing_slash=True
        ):
            findings.append(
                Finding("CONFIG_OTA_ALLOWED_URL_PREFIX", "OTA HTTPS prefix is invalid")
            )

    if parsed.values.get("CONFIG_MOTOR_INTERLOCK_ENABLED") == "y":
        if "CONFIG_MOTOR_INTERLOCK_ACTIVE_HIGH" not in policy.decisions:
            findings.append(
                Finding(
                    "CONFIG_MOTOR_INTERLOCK_ACTIVE_HIGH",
                    "release policy must record interlock polarity",
                )
            )
        _require_policy_values(policy, {"CONFIG_MOTOR_INTERLOCK_GPIO"}, findings)

    if parsed.values.get("CONFIG_TACH_FEEDBACK_ENABLED") == "y":
        tach_choices = (
            "CONFIG_TACH_STALL_ALARM_ONLY",
            "CONFIG_TACH_STALL_STOP_FAN",
            "CONFIG_TACH_STALL_STOP_ALL",
        )
        for symbol in tach_choices:
            if symbol not in policy.decisions:
                findings.append(
                    Finding(symbol, "release policy must record tach fault action")
                )
        _validate_choice(parsed, tach_choices, findings)
        _require_policy_values(
            policy,
            {
                "CONFIG_TACH_GPIO",
                "CONFIG_TACH_PULSES_PER_REV",
                "CONFIG_TACH_STALL_RPM",
                "CONFIG_TACH_STALL_DEBOUNCE_MS",
                "CONFIG_TACH_STARTUP_GRACE_MS",
            },
            findings,
        )

    if parsed.values.get("CONFIG_COMM_LOSS_STOP_AFTER_LEASE") == "y":
        _require_policy_values(policy, {"CONFIG_COMMUNICATION_LEASE_MS"}, findings)
    if parsed.values.get("CONFIG_SCHEDULE_ENABLED") == "y":
        _require_policy_values(policy, {"CONFIG_SCHEDULE_SLOTS"}, findings)
    if parsed.values.get("CONFIG_RESTORE_FAN_STATE") == "y":
        _require_policy_values(policy, {"CONFIG_FAN_STATE_SAVE_DEBOUNCE_MS"}, findings)


def validate_sdkconfig(
    parsed: ParsedSdkconfig,
    *,
    mode: str,
    policy: ReleasePolicy | None = None,
) -> list[Finding]:
    if mode not in {"template", "release"}:
        raise ValueError("mode must be template or release")
    if mode == "release" and policy is None:
        raise ValueError("release mode requires a policy")
    if mode == "template" and policy is not None:
        raise ValueError("template mode does not accept a release policy")

    findings = [
        Finding(symbol, "symbol is defined more than once")
        for symbol in parsed.duplicates
    ]
    _expect(parsed, BASELINE_EXPECTED, findings, "security baseline mismatch")
    _validate_secret_fields(parsed, mode, findings)
    _validate_choice(
        parsed,
        ("CONFIG_COMM_LOSS_CONTINUE", "CONFIG_COMM_LOSS_STOP_AFTER_LEASE"),
        findings,
    )

    if mode == "template":
        _expect(parsed, TEMPLATE_EXPECTED, findings, "template policy mismatch")
    else:
        assert policy is not None
        _expect(parsed, policy.decisions, findings, "release policy mismatch")
        for symbol, expected in policy.required_values.items():
            raw = parsed.values.get(symbol)
            if raw is None:
                findings.append(Finding(symbol, "reviewed value is absent"))
            elif _decoded_value(raw) != expected:
                findings.append(Finding(symbol, "reviewed value mismatch"))
        _validate_release_conditionals(parsed, policy, findings)

    return sorted(set(findings))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdkconfig", type=Path, required=True)
    parser.add_argument(
        "--mode", choices=("template", "release"), default="release"
    )
    parser.add_argument(
        "--policy",
        type=Path,
        help="reviewed JSON policy; required in release mode",
    )
    args = parser.parse_args(argv)

    if args.mode == "release" and args.policy is None:
        parser.error("--policy is required in release mode")
    if args.mode == "template" and args.policy is not None:
        parser.error("--policy is valid only in release mode")

    try:
        text = args.sdkconfig.read_text(encoding="utf-8")
    except OSError as exc:
        print(
            f"production configuration check could not read sdkconfig "
            f"({exc.__class__.__name__})",
            file=sys.stderr,
        )
        return 2

    policy: ReleasePolicy | None = None
    if args.policy is not None:
        try:
            policy = _read_json_policy(args.policy)
        except PolicyError as exc:
            print(f"production release policy is invalid: {exc}", file=sys.stderr)
            return 2

    parsed = parse_sdkconfig(text)
    findings = validate_sdkconfig(parsed, mode=args.mode, policy=policy)
    if findings:
        print("Production configuration policy failed; values are intentionally hidden:")
        for finding in findings:
            print(f"- {finding.display()}")
        return 1

    if args.mode == "template":
        print("Production CI template policy passed (non-deployable, no credentials).")
    else:
        print("Production release configuration policy passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
