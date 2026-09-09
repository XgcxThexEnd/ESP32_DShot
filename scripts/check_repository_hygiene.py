#!/usr/bin/env python3
"""Fail safely when generated configs or likely credentials are committable.

The checker reports only a path, line number, and variable/marker name. It never
prints the suspected value. It scans tracked files and non-ignored untracked
files, which makes it suitable for a local pre-commit check.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path, PurePosixPath


ROOT = Path(__file__).resolve().parents[1]
BLOCKED_NAMES = {"sdkconfig", "sdkconfig.old", ".env"}
ALLOWED_ENV_NAMES = {".env.example"}
BLOCKED_SUFFIXES = {
    ".key",
    ".private.pem",
    ".signing.pem",
    ".p12",
    ".pfx",
    ".jks",
    ".keystore",
    ".nvs.bin",
    ".nvs.csv",
}
SENSITIVE_CONFIG = re.compile(
    rb"^(CONFIG_(?:WIFI_(?:SSID|PASS)|MQTT_(?:BROKER_URI|USERNAME|PASSWORD)))=(.*)$"
)
PRIVATE_KEY = re.compile(
    rb"-----BEGIN (?:RSA |EC |OPENSSH |ENCRYPTED )?PRIVATE KEY-----"
)
EMBEDDED_MQTT_CREDENTIALS = re.compile(rb"mqtts?://[^\s/:@]+:[^\s/@]+@", re.I)
DOCUMENTED_TEST_URIS = {b"mqtt://user:pass@broker", b"mqtts://user:pass@broker"}


def is_blocked_filename(name: str) -> bool:
    lowered = name.lower()
    blocked_env = lowered.startswith(".env.") and lowered not in ALLOWED_ENV_NAMES
    return (
        lowered in BLOCKED_NAMES
        or blocked_env
        or any(lowered.endswith(suffix) for suffix in BLOCKED_SUFFIXES)
    )


def is_private_path(path: str) -> bool:
    casefolded = path.replace("\\", "/").casefold()
    return casefolded.startswith("secrets/") or casefolded.startswith(
        "provisioning/private/"
    )


def candidate_paths() -> list[PurePosixPath]:
    result = subprocess.run(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
        cwd=ROOT,
        check=True,
        stdout=subprocess.PIPE,
    )
    return [
        PurePosixPath(raw.decode("utf-8", errors="surrogateescape"))
        for raw in result.stdout.split(b"\0")
        if raw
    ]


def main() -> int:
    findings: list[str] = []
    try:
        paths = candidate_paths()
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"repository hygiene check could not enumerate files: {exc}", file=sys.stderr)
        return 2

    for rel in paths:
        display = rel.as_posix()
        if is_blocked_filename(rel.name):
            findings.append(f"{display}: generated config or private-key container is committable")
            continue
        if is_private_path(display):
            findings.append(f"{display}: private provisioning path is committable")
            continue

        path = ROOT.joinpath(*rel.parts)
        try:
            data = path.read_bytes()
        except (FileNotFoundError, IsADirectoryError):
            continue
        except OSError as exc:
            findings.append(f"{display}: could not inspect file ({exc.__class__.__name__})")
            continue

        for line_number, line in enumerate(data.splitlines(), start=1):
            assignment = SENSITIVE_CONFIG.match(line)
            if assignment:
                value = assignment.group(2).strip()
                if value not in {b"", b'""', b"''"}:
                    name = assignment.group(1).decode("ascii", errors="replace")
                    findings.append(f"{display}:{line_number}: non-empty {name} assignment")
            if PRIVATE_KEY.search(line):
                findings.append(f"{display}:{line_number}: private-key marker")
            embedded = EMBEDDED_MQTT_CREDENTIALS.search(line)
            if embedded and not any(example in line for example in DOCUMENTED_TEST_URIS):
                findings.append(f"{display}:{line_number}: credentials embedded in MQTT URI")

    if findings:
        print("Repository hygiene check failed; suspected values are intentionally hidden:")
        for finding in findings:
            print(f"- {finding}")
        return 1

    print("Repository hygiene check passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
