# Hardware-in-loop evidence

`evidence-manifest.template.json` is the versioned, machine-readable release
record for the checks in `docs/hardware-validation.md`. Copy it and its planned
`artifacts/` directory into a release-specific evidence directory; do not edit
the repository template with results from one device.

A draft can be checked while measurements are still pending:

```bash
python scripts/validate_hil_evidence.py \
  hil/evidence-manifest.template.json --allow-incomplete
```

For release acceptance, remove `--allow-incomplete`, set `record_status` to
`complete`, replace all metadata and limits, record every required check, and
provide each referenced file with its lowercase SHA-256 digest. Artifact paths
must be POSIX-style relative paths below the manifest directory. The strict
validator rejects failed or unrun checks, missing files, digest mismatches,
unsafe paths, duplicate GPIO assignments, and unjustified use of
`not_applicable` for feature-dependent checks.

Copy the complete packaged flash bundle into the directory containing
`FLASH-MANIFEST.json`, not only the app binary. The strict gate verifies every
schema-v1 `flash_files` entry against its matching `files` entry and the bytes
on disk, including the bootloader, partition table, OTA data, and application.
The repository's fixed production layout requires `bootloader.bin` at `0x0`,
`partition-table.bin` at `0x8000`, the tested application at `0x10000`, and
`ota_data_initial.bin` at `0x3d0000`; an app-only plan cannot pass.
It also requires the exact redacted sdkconfig and schema-v2
`RELEASE-METADATA.json`, then cross-checks source/configuration identity, the
clean-build claim, and the configuration digest. `firmware_commit` accepts a
full lowercase 40-character SHA-1 or 64-character SHA-256 Git object ID. The
resolved sdkconfig must contain parseable assignments and every sensitive value
must be empty, unset, or `<redacted>`.

The required raw evidence contract is fixed so that a generic file cannot stand
in for unrelated tests:

| Artifact ID | Required kind | Required by check(s) |
| --- | --- | --- |
| `boot-dshot-capture` | `logic_analyzer_csv` | `protocol_safety`, `startup_zero` |
| `steady-dshot-capture` | `logic_analyzer_csv` | `dshot_timing`, `protocol_safety`, `channel_isolation` |
| `interlock-capture` | `oscilloscope_capture` | `interlock_fail_safe` |
| `tach-comparison` | `tach_reference_csv` | `tach_feedback` |
| `network-recovery-log` | `mqtt_and_serial_log` | `network_recovery` |
| `ota-rollback-log` | `serial_log` | `ota_rollback` |
| `power-ordering-capture` | `oscilloscope_capture` | `power_ordering` |
| `soak-log` | `soak_csv` | `soak` |

Each required artifact must use a distinct path. Additional artifacts and
additional check-to-artifact references are allowed. Record the 16-digit
topology fingerprint reported by the tested node's `/info` topic; all-zero
template identity is rejected.

The validator establishes record completeness and artifact integrity; it does
not decide whether a waveform, temperature, current, or stop-time measurement
is safe. That acceptance decision remains an engineering review against the
limits recorded in the manifest.
