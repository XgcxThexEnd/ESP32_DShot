# Repository secret and incident-response policy

Generated ESP-IDF configuration is local state. `sdkconfig`, `sdkconfig.old`,
signing keys, device private keys, provisioning exports, and environment files
must not be committed. `.gitignore` reduces mistakes but does not protect a file
that is already tracked, staged with force, copied to an artifact, or present in
Git history.

Run this before every push and in CI:

```bash
python scripts/check_repository_hygiene.py
git status --short
git ls-files -- sdkconfig sdkconfig.old
```

The final command must print nothing. The checker intentionally prints only a
path, line, and setting/marker name; it never prints a suspected value.

Scan only the commits being proposed for a branch or pull request with:

```bash
python scripts/check_git_history_secrets.py origin/main..HEAD
```

The history scanner reads the changed blob/path pairs in the selected commits
directly and reports only paths, marker names, and abbreviated blob IDs. This
also detects an existing blob copied or renamed to a sensitive filename. Running
it with no revision scans commits reachable from every local ref and is expected
to fail until the known historical `sdkconfig` objects have been removed. Do not
make that known failure non-blocking evidence that the old credentials are safe;
rotate/revoke them first.

This is a project-specific, high-signal guardrail, not a general secret scanner.
It covers generated configuration and provisioning paths, common private-key
containers and PEM markers, uppercase credential assignments, and credentials
embedded in MQTT URLs. It does not exhaustively identify provider-specific
tokens, arbitrary structured JSON/YAML fields, or high-entropy values. Keep
hosting-platform secret scanning enabled and use provider-aware scanning during
incident response.

## Known-history response

This repository previously tracked generated configuration containing non-empty
Wi-Fi and MQTT settings. Removing those files from the current index does not
remove them from existing commits, forks, clones, caches, or build artifacts.
Treat the values as exposed even if the repository was believed to be private.

Response order:

1. Restrict repository and artifact access while the incident is assessed.
2. Rotate/revoke MQTT credentials, then rotate the Wi-Fi credential with a
   staged plan that will not strand devices. Review broker/authentication logs.
3. Remove current tracked copies without deleting operators' local files.
4. Inventory CI logs, release artifacts, forks, mirrors, backups, and developer
   clones that may contain the generated files.
5. Obtain repository-owner approval for a coordinated history rewrite.
6. Force-push cleaned references, invalidate caches, and require collaborators
   to discard/reclone affected histories rather than merge the old commits back.
7. Run secret scanning over all rewritten references and monitor for use of the
   revoked credentials.

History rewriting is destructive collaboration-wide work and is intentionally
**not** automated by this repository. A repository owner may use a reviewed tool
such as `git filter-repo` to remove the exact paths `sdkconfig` and
`sdkconfig.old`, but should prepare backups, enumerate branches/tags, communicate
the cutover, and verify the result before replacing the remote. Never place the
old values on a command line, in a commit message, or in an issue.

## Key handling

- Store firmware-signing private keys in an offline signer or HSM-backed release
  job; do not distribute them to ordinary developer workstations.
- Generate per-device private keys uniquely and prefer non-exportable storage.
- Keep development and production CAs/signing keys separate.
- Grant signing only to protected, reviewed release jobs. A build job should not
  automatically have signing authority.
- Record public certificate serials, expiry, revocation, artifact digests, and
  signer identity; never log private key material or passwords.
- Do not commit coredumps or raw provisioning/NVS images. They can contain live
  memory or credentials even when their filename looks harmless.

## Review checklist

- No generated config or private-key container is tracked.
- Configuration examples contain empty values or unmistakable placeholders.
- MQTT URLs do not embed `username:password@host`.
- Logs and screenshots are sanitized for credentials, private addresses, MACs,
  device serials, developer paths, and command payloads before commit.
- Release artifacts are versioned, hashed, signed, access-controlled, and have a
  retention policy.
- Security/eFuse procedures live in reviewed runbooks, not automatic general
  build scripts.
