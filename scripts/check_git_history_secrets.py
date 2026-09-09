#!/usr/bin/env python3
"""Scan changed Git blobs for secret markers without printing values.

The scanner reads objects directly; it never checks out an old tree and never
places matched content in a diagnostic.  By default it scans changes in commits
reachable from all local refs.  Pass one or more revision expressions to scan
only commits selected by a range such as ``origin/main..HEAD``.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


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
ASSIGNMENT = re.compile(rb"^\s*([A-Z][A-Z0-9_]{2,})\s*=\s*(.*)$")
SENSITIVE_NAME = re.compile(
    rb"(?:^|_)(?:PASSWORD|PASS|PASSPHRASE|SSID|USERNAME|SECRET|TOKEN)(?:_|$)|"
    rb"(?:^|_)(?:BROKER_URI|PRIVATE_KEY|SIGNING_KEY|CLIENT_KEY)(?:_|$)"
)
PRIVATE_KEY = re.compile(
    rb"-----BEGIN (?:RSA |EC |OPENSSH |ENCRYPTED )?PRIVATE KEY-----"
)
EMBEDDED_MQTT_CREDENTIALS = re.compile(
    rb"mqtts?://([^\s/:@]+):([^\s/@]+)@", re.I
)
DOCUMENTED_PLACEHOLDER_CREDENTIALS = {
    (b"user", b"pass"),
    (b"username", b"password"),
    (b"example", b"replace-me"),
}


@dataclass(frozen=True, order=True)
class HistoryFinding:
    path: str
    line: int
    marker: str
    kind: str
    blob_prefix: str

    def display(self) -> str:
        location = f"{self.path}:{self.line}" if self.line else self.path
        return f"{location}: {self.marker} ({self.kind}, blob {self.blob_prefix})"


class GitScanError(RuntimeError):
    """Git object enumeration or reading failed."""


def _safe_path(raw: bytes) -> str:
    decoded = raw.decode("utf-8", errors="replace")
    safe = "".join(
        char if (char.isalnum() or char in " ./_@+-") else "?"
        for char in decoded
    )
    return safe[:240] or "<unknown-path>"


def _run_git(repo: Path, arguments: list[str], *, stdin: bytes | None = None) -> bytes:
    try:
        result = subprocess.run(
            ["git", *arguments],
            cwd=repo,
            input=stdin,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    except OSError as exc:
        raise GitScanError(f"could not execute git ({exc.__class__.__name__})") from exc
    if result.returncode != 0:
        # Git stderr can include user-controlled ref/path text.  Do not echo it.
        raise GitScanError("git command failed; diagnostic content was suppressed")
    return result.stdout


def _selected_commits(repo: Path, revisions: list[str] | None) -> list[str]:
    arguments = ["rev-list"]
    arguments.extend(revisions if revisions else ["--all"])
    output = _run_git(repo, arguments)
    commits: list[str] = []
    for line in output.splitlines():
        if not re.fullmatch(rb"[0-9a-fA-F]{40,64}", line):
            raise GitScanError("git returned a malformed commit identifier")
        commits.append(line.decode("ascii"))
    return commits


def _enumerate_objects(
    repo: Path, revisions: list[str] | None
) -> dict[str, set[bytes]]:
    """Return every new-side blob path changed by the selected commits.

    Enumerating commit diffs instead of only newly reachable object IDs matters
    when a commit copies an existing blob to a sensitive path.  Rename detection
    is disabled deliberately so the destination is represented as an addition.
    """

    objects: dict[str, set[bytes]] = {}
    for commit in _selected_commits(repo, revisions):
        output = _run_git(
            repo,
            [
                "diff-tree",
                "--root",
                "-m",
                "-r",
                "--no-renames",
                "--no-commit-id",
                "--raw",
                "-z",
                "--no-abbrev",
                commit,
            ],
        )
        fields = output.split(b"\0")
        if fields and fields[-1] == b"":
            fields.pop()
        if len(fields) % 2:
            raise GitScanError("git returned malformed tree-diff data")

        for index in range(0, len(fields), 2):
            header = fields[index].split()
            path = fields[index + 1]
            if len(header) != 5 or not header[0].startswith(b":"):
                raise GitScanError("git returned malformed tree-diff metadata")
            object_id = header[3]
            if header[1] != b"160000" and re.fullmatch(
                rb"[0-9a-fA-F]{40,64}", object_id
            ) and any(
                byte != ord("0") for byte in object_id
            ):
                objects.setdefault(object_id.decode("ascii"), set()).add(path)
    return objects


def _object_metadata(repo: Path, object_ids: list[str]) -> dict[str, tuple[str, int]]:
    if not object_ids:
        return {}
    request = "".join(f"{object_id}\n" for object_id in object_ids).encode("ascii")
    output = _run_git(
        repo,
        ["cat-file", "--batch-check=%(objectname) %(objecttype) %(objectsize)"],
        stdin=request,
    )
    metadata: dict[str, tuple[str, int]] = {}
    for line in output.splitlines():
        fields = line.decode("ascii", errors="replace").split()
        if len(fields) != 3 or not fields[2].isdigit():
            raise GitScanError("git returned malformed object metadata")
        metadata[fields[0]] = (fields[1], int(fields[2]))
    return metadata


def _path_findings(path: str, blob_prefix: str) -> list[HistoryFinding]:
    normalized = path.replace("\\", "/")
    name = normalized.rsplit("/", 1)[-1].lower()
    findings: list[HistoryFinding] = []
    blocked_env = name.startswith(".env.") and name not in ALLOWED_ENV_NAMES
    if (
        name in BLOCKED_NAMES
        or blocked_env
        or any(name.endswith(suffix) for suffix in BLOCKED_SUFFIXES)
    ):
        findings.append(
            HistoryFinding(path, 0, "sensitive-file-name", "path policy", blob_prefix)
        )
    normalized_casefold = normalized.casefold()
    if normalized_casefold.startswith("secrets/") or normalized_casefold.startswith(
        "provisioning/private/"
    ):
        findings.append(
            HistoryFinding(path, 0, "private-provisioning-path", "path policy", blob_prefix)
        )
    return findings


def _nonempty_assignment(value: bytes) -> bool:
    stripped = value.strip()
    if stripped in {b"", b'""', b"''", b"(", b"[", b"{"}:
        return False
    # Pattern declarations and collection initializers commonly contain words
    # such as PRIVATE_KEY or SECRET in the constant name.  They are policy
    # metadata, not credentials.  Quoted/scalar assignments and PEM markers
    # remain covered.
    return not stripped.startswith((b"re.compile(", b"frozenset("))


def scan_blob(data: bytes, path: str, blob_prefix: str) -> list[HistoryFinding]:
    findings = _path_findings(path, blob_prefix)
    for line_number, line in enumerate(data.splitlines(), start=1):
        assignment = ASSIGNMENT.match(line)
        if assignment and SENSITIVE_NAME.search(assignment.group(1)):
            if _nonempty_assignment(assignment.group(2)):
                marker = assignment.group(1).decode("ascii", errors="replace")
                findings.append(
                    HistoryFinding(
                        path, line_number, marker, "non-empty assignment", blob_prefix
                    )
                )
        if PRIVATE_KEY.search(line):
            findings.append(
                HistoryFinding(
                    path, line_number, "private-key-marker", "content marker", blob_prefix
                )
            )
        embedded = EMBEDDED_MQTT_CREDENTIALS.search(line)
        placeholder = embedded and (
            embedded.group(1).lower(), embedded.group(2).lower()
        ) in DOCUMENTED_PLACEHOLDER_CREDENTIALS
        if embedded and not placeholder:
            findings.append(
                HistoryFinding(
                    path,
                    line_number,
                    "embedded-mqtt-credentials",
                    "content marker",
                    blob_prefix,
                )
            )
    return findings


def scan_repository(
    repo: Path,
    revisions: list[str] | None = None,
    *,
    maximum_blob_bytes: int = 5 * 1024 * 1024,
) -> list[HistoryFinding]:
    objects = _enumerate_objects(repo, revisions)
    metadata = _object_metadata(repo, list(objects))
    findings: list[HistoryFinding] = []
    for object_id, raw_paths in objects.items():
        object_type, size = metadata.get(object_id, ("missing", 0))
        if object_type != "blob":
            continue
        prefix = object_id[:12]
        if size > maximum_blob_bytes:
            for raw_path in raw_paths:
                path = _safe_path(raw_path)
                findings.extend(_path_findings(path, prefix))
                findings.append(
                    HistoryFinding(
                        path, 0, "blob-size-limit", "content not scanned", prefix
                    )
                )
            continue
        data = _run_git(repo, ["cat-file", "blob", object_id])
        for raw_path in raw_paths:
            findings.extend(scan_blob(data, _safe_path(raw_path), prefix))
    return sorted(set(findings))


def findings_fingerprint(findings: list[HistoryFinding]) -> str:
    """Stable value-free fingerprint useful for comparing CI scan results."""
    digest = hashlib.sha256()
    for finding in sorted(findings):
        digest.update(finding.display().encode("utf-8"))
        digest.update(b"\n")
    return digest.hexdigest()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "revisions",
        nargs="*",
        help="git commit/range expressions; defaults to commits on all local refs",
    )
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument(
        "--maximum-blob-bytes", type=int, default=5 * 1024 * 1024
    )
    args = parser.parse_args(argv)
    if args.maximum_blob_bytes < 1:
        parser.error("--maximum-blob-bytes must be positive")

    try:
        findings = scan_repository(
            args.repo.resolve(),
            args.revisions or None,
            maximum_blob_bytes=args.maximum_blob_bytes,
        )
    except GitScanError as exc:
        print(f"Git history secret scan could not complete: {exc}", file=sys.stderr)
        return 2

    if findings:
        print("Git history secret scan failed; matched values are intentionally hidden:")
        for finding in findings:
            print(f"- {finding.display()}")
        print(f"Finding-set fingerprint: {findings_fingerprint(findings)}")
        return 1
    print("Git history secret scan passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
