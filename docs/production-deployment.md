# Production deployment and security

This document is a deployment gate, not a promise that the current firmware is
production-ready. A DShot controller is an actuator: loss of MQTT authorization,
an unsafe update, or an unexpected restored schedule can cause physical motion.
Keep a hardware power disconnect and a mechanically safe commissioning setup.

## Current security boundary

The repository defaults intentionally keep schedules, fan-state restoration,
MQTT-outage restart, and OTA off. HTTPS OTA
uses the public CA bundle and disables redirects, but transport security alone
does not prove who built an image.

The firmware now defaults to WPA2/WPA3 plus PMF, requires MQTT TLS with non-empty
authentication, synchronizes time with SNTP before TLS, uses a full-MAC-derived
node ID when none is configured, and scopes command/state/schedule/OTA topics
under `<root>/nodes/<node-id>`. Production app rollback confirms a pending image
only after a local motor/RMT/task self-test. The remaining gaps before treating a
shared-network fleet as production-ready are:

- Wi-Fi and MQTT values are still compile-time configuration, not per-device
  provisioned records.
- The MQTT client supports username/password but not a provisioned per-device
  client certificate/private key.
- The secure transport/authentication gates can be disabled for development;
  production configuration and release review must keep them enabled.
- A full MAC or configured node label prevents accidental topic collisions but
  is not an authentication credential.
- The current command protocol has no command identifier, expiry, or persistent
  replay protection. Retained commands are rejected, which is useful but not a
  complete replay defense.
- OTA commands are per-node, but the payload still lacks a signed manifest,
  artifact version/digest, command ID, expiry, and persistent replay policy.

## Reproducible configuration layers

Use one common layer and one environment layer. The resulting local sdkconfig is
ignored and must never be committed:

- `sdkconfig.defaults`: common target, partition, TLS, and safe feature defaults.
- `configs/sdkconfig.single-fan-node.defaults`: optional one-ESC-per-node layer;
  review its GPIO before using it.
- `configs/sdkconfig.dev.defaults`: diagnostic development build.
- `configs/sdkconfig.production.defaults`: reversible production build settings.
- `sdkconfig.<environment>.local`: untracked credentials and device-local choices.

For a fresh production build in PowerShell:

```powershell
idf.py -B build-production `
  -D "SDKCONFIG=sdkconfig.production.local" `
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;configs/sdkconfig.production.defaults" `
  set-target esp32s3
idf.py -B build-production `
  -D "SDKCONFIG=sdkconfig.production.local" `
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;configs/sdkconfig.production.defaults" `
  menuconfig build
python scripts/check_repository_hygiene.py
```

Defaults are applied when the selected local sdkconfig is first created. Delete
only that specifically named local file when intentionally regenerating it; do
not delete a shared workspace or an operator's existing `sdkconfig`.

The production layer does **not** burn or request security eFuses. A successful
production build is therefore not evidence that Secure Boot or flash encryption
is active on a device. Record and verify the generated configuration and the
read-only eFuse summary for each manufactured unit.

Validate the resolved credential-free CI profile with:

```bash
python scripts/check_production_sdkconfig.py \
  --sdkconfig build-production/sdkconfig --mode template
```

Before creating a deployable artifact, copy and review
`configs/production-release-policy.example.json`, then run the same checker in
`release` mode against the exact provisioned sdkconfig. The policy records every
deployment-dependent interlock, tach, communication-loss, restoration,
schedule, OTA, signing, encryption, and anti-rollback choice without containing
credentials. See [`fault-response-policy.md`](fault-response-policy.md) for the
required hazard decisions. The example values are not a board wiring approval.

## Per-device identity and provisioning

Use a manufacturing identity that is unique, stable, and bound to a credential.
A recommended record contains:

- an opaque device UUID or public-key fingerprint;
- the full factory MAC and effective node ID for inventory;
- location, controller/fan index, GPIO, board revision, and hardware batch;
- MQTT certificate serial/credential ID and its issue/expiry dates;
- firmware version, image digest, security version, and configuration revision;
- provisioning, rotation, revocation, and decommission timestamps.

Generate device private keys on the device, a secure element, or an isolated
provisioning station. Never place a private key in source control, ordinary CI
logs, a shared firmware image, or an operator spreadsheet. Prefer keys that are
non-exportable. Store exportable credentials only after flash/NVS encryption has
been designed and validated.

The current application does not load MQTT client certificates or secrets from
an encrypted provisioning partition. Implement and review that path before
moving credentials out of Kconfig. A robust design uses two credential slots so
the fleet can install and verify a replacement before revoking the old one.

Provisioning flow:

1. Mechanically isolate the fan and verify the hardware power disconnect.
2. Read the full hardware identity and reject duplicate inventory records.
3. Generate or inject one device credential; never reuse a device private key.
4. Install only the CA chain needed for the broker/update service.
5. Bind the authenticated identity to a deny-by-default broker ACL.
6. Flash a versioned, verified artifact and record its digest.
7. Confirm zero-throttle startup, identity, TLS verification, LWT, and state.
8. Remove manufacturing access and seal or disable debug paths according to the
   approved device-recovery policy.

Provide a physically gated recovery mode for network credential replacement.
If SoftAP or BLE provisioning is introduced, make it time-limited, possession-
authenticated, disabled after enrollment, and unable to start a motor.

## MQTT authorization policy

Use unique device principals. A controller should normally publish only inside
its own node subtree and subscribe only to its own commands. The automation/
control-plane principal should be separate from device principals; OTA
publishing should be a still narrower release role. Home Assistant discovery is
an explicit exception only when `HOME_ASSISTANT_DISCOVERY_ENABLED` is selected.

Current node-scoped topic model:

```text
greenhouse_esp/nodes/<device-id>/fan<index>/...
greenhouse_esp/nodes/<device-id>/health
greenhouse_esp/nodes/<device-id>/info
greenhouse_esp/nodes/<device-id>/announce
greenhouse_esp/nodes/<device-id>/status
greenhouse_esp/nodes/<device-id>/ota/update
greenhouse_esp/nodes/<device-id>/ota/status
```

For the node-scoped protocol:

- give each node a unique broker credential and ACL its exact node subtree;
- deny device principals permission to publish actuator/OTA commands;
- deny retained command publications at the broker as defense in depth;
- authorize schedule and OTA command leaves only for the control/release roles
  that need them;
- leave firmware-owned Home Assistant discovery disabled for a strict subtree-
  only device ACL; or grant publish-only access to exactly
  `homeassistant/fan/<node-id>_fan<index>/config` and, when tach feedback is
  enabled, `homeassistant/sensor/<node-id>_fan<index>_rpm/config`, enumerating
  each local fan index because an MQTT `+` wildcard cannot match part of a
  topic level;
- never grant the device subscribe permission on the Home Assistant discovery
  tree, and remove its retained discovery documents during decommissioning;
- never use an anonymous listener or plaintext MQTT outside an isolated bench
  network.

`MQTT_LEGACY_TOPICS` is a temporary migration switch, not a production default.
Legacy mode reintroduces global fan and schedule topics and therefore requires
globally unique fan indices 1–8 plus additional ACLs. OTA always remains
node-scoped. Remove old retained discovery/state and turn the option back off
after migration.

Broker ACL syntax differs, so test both publish and subscribe authorization with
the exact broker/version. A connection succeeding is not proof that an ACL is
correct. Verify denied cross-device fan commands, schedule writes, retained
commands, discovery writes, and OTA commands.

## Image signing, Secure Boot, and flash encryption

Use separate development and production trust domains. The production signing
private key belongs in an offline signer or HSM-backed protected release job;
only its verification digest belongs on devices. Back up keys and recovery
instructions under dual control, and exercise key revocation before deployment.

Recommended staged adoption:

1. Produce versioned artifacts, hashes, dependency/SBOM records, and build
   provenance without changing eFuses.
2. Rehearse signed-image verification on disposable development hardware.
3. Exercise the implemented application rollback health confirmation and add a
   signed recovery image.
4. Verify every bootloader, partition table, factory image, and OTA image in the
   release set is compatible with the chosen security policy.
5. Enable Secure Boot V2 and flash encryption on a sacrificial production-like
   unit, power-cycle it repeatedly, test OTA and recovery, and read back its
   eFuse summary.
6. Only then approve a controlled manufacturing operation for production units.

Do not copy eFuse-burning commands into a general build script. Burning Secure
Boot, encryption, debug-disable, download-mode, or anti-rollback eFuses can be
irreversible; a wrong key, unsigned recovery image, or lost encryption material
can permanently prevent recovery. This repository intentionally provides no
automatic burn command.

Flash-encryption keys should be unique per device. The firmware-signing key can
be centrally controlled, but should not be present on devices. Decide whether
UART download and JTAG remain available for field recovery; leaving them open
weakens resistance to physical access, while disabling them makes depot recovery
harder or impossible.

## Rollback and anti-rollback gate

The partition table already supplies a factory image, two OTA slots, and
`otadata`. The production defaults enable bootloader app rollback, and the
application confirms a pending image only after meaningful local health checks.

The confirmation gate should verify at least:

1. the new image reached application startup;
2. NVS/configuration initialization completed or entered a reported safe-reset
   state;
3. configured motor outputs established the zero-throttle arming stream;
4. required motor/control tasks were created and remain healthy;
5. the application can remain safely operational without Wi-Fi or MQTT.

Do not require broker connectivity to mark an image valid; a broker outage would
otherwise roll back healthy devices. Publish post-boot version and validation
status later when connectivity is available.

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` is now in the production layer. Keep it
covered by hardware-in-loop tests for successful confirmation and forced local
self-test failure. Anti-rollback/security-version eFuses come later: keep a tested
signed recovery image at an allowed security version, define how emergency
releases increment the version, and accept that an anti-rollback device cannot
boot an older vulnerable image even for diagnosis.

## OTA release and canary runbook

The current URL-prefix check and HTTPS transport are not an artifact-signing or
rollout system. A production control plane should use a signed manifest with an
artifact digest, application/security version, target/cohort, command ID, and
replay/downgrade policy.

For every release:

1. Confirm the artifact signature and digest independently of its download host.
2. Record compatibility, partition-size headroom, configuration migration, and
   recovery requirements.
3. Test install, loss of power during download, first-boot failure, automatic
   rollback, and broker loss on a hardware-in-loop rig.
4. Start with a mechanically safe canary and require a post-boot validated
   version report; a pre-reboot `success` message is insufficient.
5. Expand to small cohorts while watching offline rate, reset/rollback count,
   motor-control errors, and version convergence.
6. Stop automatically when thresholds are exceeded. Preserve the prior signed
   artifact and a recovery procedure.
7. Revoke the temporary OTA authorization after the rollout.

Use the node-scoped OTA topic and a release-role ACL to target exactly one canary.
The firmware does not accept a legacy global OTA topic.

CI's unsigned evidence bundle now includes the app, bootloader, partition table,
initial OTA data, a portable offset/hash manifest, the ELF/map, dependency lock,
size report, redacted configuration, source commit, builder identity, and
path-free ESP-IDF project metadata. This makes a build traceable and locally
recoverable; it does **not** make it authentic. `RELEASE-METADATA.json` continues
to declare `production_ota_suitable: false` until a separately protected signing
workflow and device-side verification policy are implemented.

## Partition decision record

The current 4 MiB layout is intentionally unchanged:

- Two OTA slots plus `otadata` are already present.
- An `nvs_keys` partition is not useful until encrypted NVS provisioning is
  implemented and its key-generation/recovery flow is defined.
- A coredump partition can improve diagnosis but can also retain MQTT material,
  command payloads, addresses, and other memory. Do not add one until access,
  encryption, redaction, retention, extraction, and erase procedures exist.
- Adding either partition reduces OTA growth/recovery headroom. Recalculate all
  offsets and signed-image size margins together rather than consuming the
  remaining flash piecemeal.

Review this decision whenever credentials move to NVS, crash capture is added,
the application approaches an OTA slot limit, or the flash size changes.
