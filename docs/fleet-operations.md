# Fleet operations runbook

This runbook describes the current MQTT protocol and the controls needed to run
multiple physical fan nodes safely. It does not replace a hardware emergency
stop. Anyone with permission to publish a command topic can request motion.

## One fan per ESP32 at scale

For each one-fan controller, configure:

- `FAN_COUNT=1`;
- one output-safe GPIO in `DSHOT_GPIO`;
- a unique `NODE_ID`, or a blank value to derive one from the full base MAC;
- a unique MQTT credential for the node-scoped client ID/topic tree;
- a displayed `FAN_INDEX_START` value (it may be `1` on every node when legacy
  topics are disabled);
- restoration, schedules, broker-outage restart, and OTA only when explicitly
  approved for that device class.

Example inventory:

| Device UUID/node ID | Full MAC | Fan index | GPIO | Location | MQTT credential ID | Firmware digest |
| --- | --- | --- | --- | --- | --- | --- |
| unique and ACL-bound | recorded, not used as a secret | local display index | board-checked | physical asset | unique and revocable | release record |

Current command topics include the node ID:
`<root>/nodes/<node-id>/fanN/...`. A configured node ID must be unique; a blank
one uses `esp32s3_<full-base-MAC>`. The label is still public and spoofable, so
bind its subtree to an authenticated per-device username/password broker
credential. Per-device client certificates remain a future provisioning
hardening step and are not loaded by the current firmware.
Only legacy-topic migrations require a fleet-wide fan-index registry (range
1–8).

At steady state a one-fan node publishes reported/requested/applied state, command
and RMT counters, plus node health about every five seconds: roughly two QoS
publishes/second before tach data, command responses, and optional reconnect
discovery.
Measure the exact build and size broker connections, retained storage, ACL
evaluation, and monitoring for the fleet total. Stagger commissioning/power
restoration to avoid simultaneous reconnect storms and, when firmware-owned
discovery is enabled, discovery storms.

Only power-on, planned software, and deep-sleep resets are eligible to restore a
persisted motion request. After every other reset reason, firmware commits zero
fan targets and a schedule inhibit before enabling output; investigate the reset
and explicitly re-enable schedules only after the node is verified.

## Onboarding checklist

1. Mechanically isolate the fan and verify its physical power disconnect.
2. Allocate and record a device UUID/node ID, full MAC, displayed fan index, GPIO,
   credential ID, location, and expected firmware digest.
3. Verify the broker rejects another device's node subtree in both directions.
4. Boot with restoration and schedules off; confirm a sustained zero state.
5. Verify retained availability becomes `online` and LWT changes it to `offline`
   after an ungraceful disconnect.
6. Command a bounded nonzero value on a safe rig, verify a fresh applied-state
   observation, then command zero and verify a fresh zero.
7. Reboot and confirm the approved restoration/schedule policy.
8. Record the observed node-info payload and baseline refresh-counter behavior.

When `HOME_ASSISTANT_DISCOVERY_ENABLED` is selected, discovery names include
node ID and fan index. If either changes, clear the old retained discovery
record deliberately so stale entities cannot continue targeting a reassigned
fan.

## Signals currently available

| Signal | Meaning | Important limitation |
| --- | --- | --- |
| `<node>/status` | retained `online` plus broker LWT `offline` | node label must also be ACL-bound |
| `<node>/fanN/requested_percentage` | requested manual/schedule target | can differ from applied output |
| `<node>/fanN/applied_percentage` | active RMT percentage | not proof of mechanical rotation |
| `<node>/fanN/measured_rpm` | valid independent tach sample | present only when tach feedback is enabled/wired |
| `<node>/fanN/rmt_refresh_count` and error metrics | RMT activity/failures | positive refresh change is not proof the fan rotates |
| `<node>/health` | heap, MQTT ACK/outbox, Wi-Fi, configured/active interlock state, fail-safe, completed-stop tuple, stacks, topology | alert thresholds are deployment-specific; `stop_in_progress` distinguishes active work from the last completed result |
| `<node>/info` | app/IDF version, partition, reset reason, boot health, flags, topology fingerprint | add artifact digest/config revision in external inventory |
| `<node>/ota/status` | coarse OTA lifecycle | `downloaded_rebooting` is not post-boot health |

Alert at minimum on stale availability/metrics, duplicate client IDs, repeated
reconnects, refresh counters that stop changing, RMT errors, unexpected
requested/applied differences, communication/global safety latches, version
skew, and OTA failure. Without the independent tach option there is no mechanical
rotation proof; with it, alert on missing valid samples, low RPM, and stall alarm.

## Safe stop and maintenance

Before maintenance:

1. Publish schedule override `1` if schedules are enabled.
2. Publish percentage `0` for every allocated fan index.
3. Require a fresh post-command zero percentage for every fan.
4. Remove motor power physically and verify it is absent.

MQTT confirmation is not an emergency stop. Network loss, process failure, a
stale retained state, or an active local schedule can invalidate software-only
cleanup.

## Broker outage

The MQTT library reconnects automatically, and the default configuration does
not reboot on broker loss. The default communication policy continues local
motor output/schedules; deployments may instead select a lease that latches
motors off after confirmed MQTT health is lost.

1. Determine whether the outage affects control, authentication, DNS, TLS, or
   the broker itself; do not repeatedly power-cycle actuators as a diagnostic.
2. Use LWT and broker logs to identify the last known device session.
3. If state cannot be trusted, isolate motor power before on-site work.
4. Restore the broker, check for duplicate client IDs, and require fresh state
   and refresh metrics rather than accepting retained values alone.
5. Verify broker policy rejected retained commands while clients were offline.

## Wi-Fi credential rotation

The present firmware compiles Wi-Fi credentials into each local sdkconfig and
has no remote dual-slot rotation. Changing the AP credential first can strand
the device.

1. Inventory every affected device and arrange safe physical access.
2. If possible, overlap old/new SSIDs or credentials during migration.
3. Build from a protected local configuration without committing it.
4. Update one canary locally, verify reconnect and safe motor state, then expand.
5. Retire the old network only after every device is observed on the replacement.

Implement authenticated dual-slot provisioning before attempting unattended
fleet-wide Wi-Fi rotation.

## MQTT credential or certificate rotation

Use a unique principal per device so one compromise can be revoked in isolation.
With a future dual-slot provisioning implementation:

1. Issue a replacement with a short overlap and the same least-privilege ACL.
2. Install it over an authenticated channel and verify a new session using it.
3. Revoke the old credential and verify reconnect still succeeds.
4. Alert on any later use of the revoked identity.
5. Update inventory with serial, expiry, and revocation evidence.

The current compile-time username/password path cannot perform that sequence
remotely; use the same canary/local-update process as Wi-Fi rotation.

## OTA canary, rollback, and recovery

Follow `production-deployment.md` before enabling OTA. In particular, require
signed images, a post-boot validation report, and tested rollback. OTA remains
node-scoped even when legacy fan/schedule migration is enabled.

1. Authorize only the release principal to publish the intended node's scoped
   OTA topic; keep legacy topics disabled.
2. Confirm all fans are freshly observed at zero and physical isolation exists.
3. Publish one non-retained update request and record the release/artifact ID
   externally; the current payload itself has no command ID.
4. Do not count retained `downloaded_rebooting` as completion. Confirm the device
   reconnects and reports the expected running image through an independent
   inventory/serial check.
5. Stop rollout on unexpected reset, offline duration, rollback, state, or
   refresh behavior.
6. Verify the persisted schedule override remains inhibited; only publish `0`
   after the updated or recovered node has passed the operational checks.
7. Revoke temporary OTA permission and preserve logs without credentials.

Production builds enable bootloader rollback. Confirm both pending-image
validation and forced local-self-test rollback on hardware. A signed local
reflash remains the recovery path when neither slot can boot; test it before any
irreversible eFuse change.

## NVS reset or configuration loss

The firmware can erase NVS after selected initialization/version errors. This
can remove persisted schedules and optional fan targets.

1. Treat an unexpected configuration reset as a fleet event, not silent repair.
2. Keep motor power isolated until schedule/restoration policy is re-established.
3. Restore only validated configuration from inventory/control-plane state.
4. Record the reset reason and inspect flash health before returning the unit.

## Compromise and decommissioning

For a suspected device compromise:

1. Isolate motor power and the device network path.
2. Revoke its MQTT identity and deny its topics.
3. Preserve broker/authentication and release records without copying secrets
   into an issue or chat.
4. Determine whether credentials were device-unique. If shared, rotate every
   affected device and treat the scope as fleet-wide.
5. Reprovision from a known signed image or retire the unit.

For decommissioning, revoke credentials first, remove retained availability,
state, and node topics plus any Home Assistant discovery documents published by
nodes with `HOME_ASSISTANT_DISCOVERY_ENABLED`; erase confidential device storage
under the approved procedure, and release a fan index only after old retained
commands/entities have been removed.
