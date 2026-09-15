# Greenhouse ESP DShot Fan Controller

ESP-IDF firmware for an ESP32-S3 that controls one to four DShot600 ESC-driven
fans. It provides node-scoped MQTT control, optional Home Assistant discovery,
smooth ramping, safety supervision, optional tachometer feedback,
uptime-relative schedules, and opt-in rollback-aware HTTPS OTA updates.

> [!WARNING]
> DShot commands can start a motor immediately. Remove propellers/blades or
> mechanically isolate the load during setup and testing. Power ESCs from a
> correctly rated supply, connect the ESP32 and ESC grounds, and do not treat
> MQTT or test cleanup as an emergency stop. Fan-state restoration is optional;
> if you enable it, a reboot can restore a nonzero target from NVS. A nonzero
> target is restored only when the persisted node/motor topology fingerprint
> still matches the configured node ID, fan range, and ordered GPIO mappings.
> Only power-on, planned software, and deep-sleep resets are eligible to
> restore motion. Every other reset reason instead overwrites restorable
> targets with zero and persistently inhibits schedules before output can be
> enabled.

## Features

- Fixed DShot600 output through ESP-IDF RMT, continuously repeated for stable
  throttle.
- One to four fans, matching the ESP32-S3's four RMT TX channels, with strict
  GPIO validation.
- Configurable minimum spin speed and smooth ramp step/timing.
- Node-scoped MQTT control and reported state/health, plus optional retained
  Home Assistant discovery. A blank node ID derives a stable ID from the full
  base MAC.
- Separate requested and actually applied percentages, RMT refresh/error
  counters, and accepted-command counts.
- Optional independent tachometer inputs with measured RPM, stall alarms, and a
  configurable stop-one/stop-all policy.
- Optional hardware motor-enable/interlock output, held inactive through local
  boot self-test and deasserted on safety faults.
- Motor-task watchdog/supervisor and optional communication-lease fail-safe.
- Optional fan-target persistence and restart restoration; disabled by default.
- Optional uptime-relative repeating schedules; disabled by default.
- Optional restart after a prolonged MQTT outage; disabled by default because
  the MQTT client already reconnects automatically.
- MQTT-triggered HTTPS OTA support; disabled by default and restricted to a
  configured trusted URL prefix. Production builds enable bootloader rollback;
  a pending image is confirmed only after local motor/RMT/task self-test.
- WPA2/WPA3, PMF, MQTT TLS/authentication, certificate-date validation, and SNTP
  time synchronization are the default security policy.

Experimental bidirectional DShot telemetry remains hidden and disabled. Use the
independent PCNT tachometer path when measured RPM or stall action is required;
it must be wired and enabled explicitly.

## Firmware architecture

`app_main.c` is the boot-order and dependency coordinator. Long-lived state is
owned by focused modules instead of shared controller globals:

| Module | Ownership |
| --- | --- |
| `app_config.c` | validated immutable node/fan/GPIO configuration and topology fingerprint |
| `wifi_manager.c` | station events, reconnect backoff, IP state, and SNTP |
| `mqtt_manager.c` | client lifecycle, subscriptions, bounded RX dispatch, session commit fencing, and ACK health |
| `command_router.c` | side-effect-free MQTT topic/payload validation and typed commands |
| `command_executor.c` | typed command execution, session authorization, and output transaction ordering |
| `motor_control.c` | DShot/RMT channels, zero stream, ramping, targets, and motor inhibits |
| `tach_monitor.c` | PCNT sampling, RPM, debounce, fault generations, and stall latches |
| `tach_actions.c` | supervised fault-stop worker and serialized tach alarm clearing |
| `scheduler.c` | schedule state, runtime activation, topology-gated NVS persistence, and bounded commit ACKs |
| `fan_state_store.c` | topology-gated manual-target persistence and all-zero-only legacy migration |
| `ota_manager.c` | singleton verified-HTTPS update lifecycle |
| `state_publisher.c` | Home Assistant discovery, state, metrics, ACKs, and health reports |
| `safety_supervisor.c` | hardware interlock, task/action watchdogs, communication lease, and durable fault stops |

Modules exchange immutable snapshots and typed callbacks. Motor/tach/scheduler
locks remain private, and MQTT or NVS work is never performed while those locks
are held. This also keeps one-fan-per-ESP nodes and four-output nodes on the same
firmware architecture; deployment scale is configured through `FAN_COUNT`,
`FAN_INDEX_START`, and the node-scoped identity.

MQTT command fragments are copied into a bounded FIFO and executed on a
dedicated dispatch task. Every side-effecting command revalidates its connection
generation at the mutation point; queue saturation or reconnect invalidates the
session before the communication fail-safe clears and commits targets. Tach
sampling similarly hands confirmed faults to a separately supervised action
task, so NVS/scheduler work cannot hide a dead PCNT sampling loop or be silently
left pending.

## Requirements and wiring

- ESP32-S3 development board.
- ESP-IDF 6.1.0 for ESP32-S3 releases. `configs/release-target.json` binds the
  dependency lock and CI builder image to this target. Builds with other SDKs
  are compatibility checks and must not replace the committed release lock.
- DShot600-compatible ESCs and an appropriately rated external motor supply.
- MQTT broker; Home Assistant is optional.

Each configured DShot GPIO connects to one ESC signal input. Connect a common
ground between the ESP32 and each ESC. Verify the selected pins are output-safe
for your exact ESP32-S3 board and are not used by flash, PSRAM, USB, or another
peripheral. Optional tach inputs must be clean ESP32-safe logic levels; an
optional interlock GPIO must control a suitably rated isolated enable, relay, or
load-switch circuit rather than motor current directly.

## Configure

Select the target before configuring a fresh checkout:

```bash
idf.py set-target esp32s3
idf.py menuconfig
```

Settings are under **Greenhouse ESP DShot ESC Controller** in
[`main/Kconfig`](main/Kconfig). Configure at least:

- Wi-Fi SSID/password, secure-authentication/PMF policy, and SNTP server.
- MQTT broker URI, username/password, root topic, and stable node ID. The default
  policy requires `mqtts://` plus non-empty authentication and verifies public
  CA certificates after SNTP establishes valid wall time.
- Fan count (one through four), first displayed fan index, and one
  comma-separated DShot GPIO per fan.
- Minimum spin percentage and ramp step/tick.
- Optional tach GPIOs/pulses-per-revolution/stall policy and optional independent
  motor interlock.
- RMT error threshold, supervisor watchdog, and whether communication loss
  continues local control or latches motors off after a lease.
- Whether fan targets may be restored after reboot (default: off).
- Whether a prolonged MQTT outage may restart the controller (default: off) and,
  if enabled, its timeout.
- Whether uptime-relative schedules should be enabled (default: off). Once
  configured, a schedule can start its fan after the interval.
- Whether MQTT-triggered OTA should be enabled (default: off) and its exact
  trusted HTTPS URL prefix, which must end with `/`.
- Whether legacy global MQTT topics should be mirrored during a bounded
  migration (default: off).
- Whether the node should publish Home Assistant discovery documents outside
  its normal MQTT subtree (default: off; requires additional publish-only ACLs).

Generated `sdkconfig*` files are intentionally untracked; common non-secret
defaults live in `sdkconfig.defaults`, with reversible development and production
layers under `configs/`. Keep credentials out of commits and logs. This
repository previously tracked generated configuration, so deployments must
follow the rotation and history-remediation steps in
[`docs/repository-security.md`](docs/repository-security.md). Treat access to the
controller's MQTT command topics as actuator access; access to the OTA topic is
equivalent to firmware-update permission.

For production, do not treat a successful build as security provisioning. Read
[`docs/production-deployment.md`](docs/production-deployment.md) before enabling
OTA, rollback, image signing, Secure Boot, flash/NVS encryption, client
certificates, or irreversible eFuses. The repository does not automatically burn
eFuses.

## Build, flash, and monitor

```bash
idf.py fullclean build
idf.py -p /dev/ttyACM0 flash monitor
```

Replace the serial port with the correct device for your system. Quit the
monitor with `Ctrl+]`.

## MQTT contract

Let `P` be `<MQTT_ROOT_TOPIC>/nodes/<NODE_ID>`. The default root is
`greenhouse_esp`; when `NODE_ID` is blank, the firmware derives a stable value
like `esp32s3_A1B2C3D4E5F6` from the full base MAC. For displayed fan index `N`:

| Topic | Direction | Payload |
| --- | --- | --- |
| `P/fanN/set` | command | exactly `ON` or `OFF` |
| `P/fanN/percentage/set` | command | decimal integer `0` through `100` |
| `P/fanN/state` | reported | `ON` or `OFF`, based on applied output |
| `P/fanN/percentage` | reported | applied percentage used by Home Assistant |
| `P/fanN/requested_percentage` | reported | current manual/schedule request |
| `P/fanN/applied_percentage` | reported | percentage represented by the active RMT stream |
| `P/fanN/accepted_command_count` | metric | monotonic accepted manual-command count |
| `P/fanN/rmt_refresh_count` | metric | monotonic successful refresh count |
| `P/fanN/rmt_error_total` | metric | cumulative RMT application errors |
| `P/fanN/rmt_error_consecutive` | metric | current consecutive RMT error count |
| `P/health` | reported | heap, MQTT ACKs/outbox, Wi-Fi, configured/active interlock state, fail-safe and stop status/result, task stacks, and topology fingerprint |
| `P/info` | inventory | app/IDF version, partition, reset reason, boot health, feature flags, and topology fingerprint |
| `P/fans` | inventory | local displayed fan indexes |
| `P/announce` | event | non-retained node-up inventory event |
| `P/status` | availability | retained `online`; MQTT last will publishes `offline` |

`ON` selects the configured minimum-spin percentage; `OFF` selects zero. A
percentage must contain only ASCII digits, with no sign or surrounding
whitespace. Malformed, empty, negative, and above-100 commands are rejected
without changing the fan target. Retained messages are rejected on every
subscribed command topic so an old command cannot replay after reconnection.
Requested and applied values can differ while ramping, after minimum-spin
clamping, or when a safety policy rejects/stops output. Treat applied state as
electrical output confirmation, not proof of mechanical rotation.

With `TACH_FEEDBACK_ENABLED`, `P/fanN/measured_rpm` is published only for a
valid PCNT sample no older than `TACH_SAMPLE_MS + 500` milliseconds. At the next
publication, an invalid or stale sample clears the retained RPM value and sets
retained `P/fanN/rpm_status` to `offline`; recovery sets it to `online`.
`P/fanN/tach_valid` and `P/fanN/tach_sample_age_ms` expose validity and age
(`-1` before a sample exists). Home Assistant RPM discovery requires both node
and RPM availability. After a counter read error, the first successful read
establishes a new baseline; RPM resumes on the following complete interval.
`P/fanN/alarm` reports the tach stall alarm, and publishing
exact `CLEAR` to `P/fanN/alarm/clear` clears the latch. Configure pulse count,
startup grace, threshold, debounce, and action for the actual fan. The node
publishes retained availability at `P/status`. When
`HOME_ASSISTANT_DISCOVERY_ENABLED` is selected, retained discovery is published
to exact `homeassistant/fan/<NODE_ID>_fanN/config` topics and, with tach enabled,
`homeassistant/sensor/<NODE_ID>_fanN_rpm/config` topics.

Health JSON uses `*_stack_bytes` for ESP-IDF stack high-water marks. Update
dashboards that read the former `*_stack_words` keys; the numeric values are
unchanged because ESP-IDF already reports bytes. `mqtt_rx_age_ms` is diagnostic
receive activity. Only accepted SUBACKs, PUBACKs, and the explicit startup
baseline renew `mqtt_ack_age_ms`; incoming commands do not extend that lease.

### One-fan multi-node deployments

For one fan per ESP32, configure `FAN_COUNT=1`, one board-safe DShot GPIO, and a
unique `NODE_ID` (or leave it blank to use the full-MAC-derived default). Fan
index `1` may be reused across node-scoped topic trees. Assign a unique MQTT
username/password credential per node and ACL it to that node's exact `P/#`
subtree. Client-certificate loading is a future provisioning hardening step; it
is not implemented by this firmware yet.
If firmware-owned Home Assistant discovery is enabled, also grant publish-only
access to each exact discovery topic for that node; otherwise leave discovery
disabled and let the control plane own it.

The non-secret [`configs/sdkconfig.single-fan-node.defaults`](configs/sdkconfig.single-fan-node.defaults)
is a ready starting layer. Review its placeholder GPIO for every board, then
layer either the development or production defaults and keep that node's
credentials in its ignored local `sdkconfig` file.

For a fresh one-fan production configuration in PowerShell:

```powershell
idf.py -B build-single-production `
  -D "SDKCONFIG=sdkconfig.single-production.local" `
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;configs/sdkconfig.single-fan-node.defaults;configs/sdkconfig.production.defaults" `
  set-target esp32s3
idf.py -B build-single-production `
  -D "SDKCONFIG=sdkconfig.single-production.local" `
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;configs/sdkconfig.single-fan-node.defaults;configs/sdkconfig.production.defaults" `
  menuconfig build
```

The onboarding checklist, inventory fields, broker sizing, safe-stop procedure,
credential rotation, and decommissioning steps are in
[`docs/fleet-operations.md`](docs/fleet-operations.md).

### Legacy-topic migration

`MQTT_LEGACY_TOPICS` is off by default. Enabling it temporarily mirrors the old
`<root>/fanN/...` and schedule topics while node-scoped control remains
authoritative. When enabled, Home Assistant discovery remains node-identified.
OTA remains node-scoped even in legacy mode. Legacy fan/schedule
mode requires globally unique fan numbers in the range 1–8 and restrictive ACLs.
Migrate controllers/automations to `P/...`, remove old retained discovery/state,
confirm no legacy publishers remain, then rebuild with legacy topics off.

### Schedules

Schedules repeat relative to device uptime; they are not wall-clock calendar
schedules. Slot numbers run from zero to `CONFIG_SCHEDULE_SLOTS - 1`.

| Topic | Payload/result |
| --- | --- |
| `P/schedule/N/S/set` | `interval_min,duration_min,target_pct` |
| `P/schedule/N/S/disable` | disables slot `S` |
| `P/schedule/N/S/get` | publishes retained values to `P/schedule/N/S` |
| `P/schedule/override` | `1` inhibits all schedules; `0` enables them |

Intervals and durations must be positive whole minutes, duration must not exceed
interval, and the target must be from 0 through 100. The comma-separated payload
is exact: signs and whitespace are not accepted. Persisted schedules load only
when their schema and node/motor topology fingerprint match; older or
incompatible schedule records are ignored and must be configured again.

### OTA

OTA is disabled by default. When enabled, publishing a non-retained firmware URL
to `P/ota/update` starts an update only when the URL begins with the configured
`OTA_ALLOWED_URL_PREFIX` and contains no ambiguous encoded/dot path segments.
The prefix must start with `https://` and end with `/`; TLS uses ESP-IDF's public
CA bundle, certificate dates are checked after SNTP sync, and only HTTP 200
responses are accepted; redirects are rejected before writing flash. Network
operations have a 10-second idle timeout and the transfer has a five-minute
deadline, including slow header/body delivery. Initial DNS resolution also
depends on the SDK's bounded resolver timeout. A failed or timed-out download
releases the update operation so fresh commands can be accepted again.
Before download, the controller inhibits schedules, deasserts motion,
requires every motor to stop, and synchronously commits both the schedule
inhibit and the latest restorable fan targets. The safety-owned schedule
inhibit cannot be cleared for the rest of that boot, even when the download
fails; after reboot, an operator may explicitly publish `0` to
`P/schedule/override` after verifying the node. Lifecycle results are published
to `P/ota/status`.

`downloaded_rebooting` confirms download only. With the production configuration,
the bootloader starts the new slot as `PENDING_VERIFY`; firmware marks it valid
only after NVS, interlock, RMT zero stream, watchdog, motor task, and optional
schedule/tach task initialization pass. A local self-test failure marks the image
invalid and requests rollback. `P/info` reports the running partition and
`boot_health` after reconnect.

Restrict the OTA topic with broker ACLs. TLS authenticates the download server,
but it does not by itself prove who built the firmware. The production runbook
covers signed application images, ESP32 Secure Boot, anti-rollback planning, and
staged per-node canary rollout. Bootloader app rollback is reversible and enabled
by the production defaults; irreversible eFuse-backed protections are never
enabled automatically.

## Tests

Install the Python tooling:

```bash
python -m pip install -r tools/requirements.txt
```

Run offline unit tests with:

```bash
python -m pytest
```

Host C tests also exercise scheduler safety races, schedule persistence,
OTA redirect rejection, incomplete/invalid images, and transport deadlines
using deterministic task, flash, and network substitutes:

```bash
python scripts/run_host_policy_tests.py --sanitize
```

On Windows, run from a Visual Studio developer shell with `--cc cl` and omit
`--sanitize`. These tests execute the production scheduler and OTA modules;
hardware validation is still required.

The offline suite includes synthetic edge-capture tests for the standalone
DShot600 waveform validator. Before connecting a motor, export a logic-analyzer
edge CSV and follow the zero-throttle, timing, wire-order, interlock, tach, fault,
and soak procedure in
[`docs/hardware-validation.md`](docs/hardware-validation.md). For example:

```bash
python scripts/analyze_dshot_capture.py captures/fan1.csv \
  --time-unit s --minimum-frame-gap-us 40
```

Live MQTT stress tests are skipped unless explicitly enabled. They send random
commands up to 100%, so use only a mechanically safe test rig. They require the
flashed controller and broker to be reachable and require fresh state/metric
observations; connecting to an empty broker is not considered success.

For example, in PowerShell:

```powershell
$env:MQTT_BROKER = "mqtts://broker.example:8883"
$env:MQTT_USERNAME = "test-user"
$env:MQTT_PASSWORD = "replace-me"
$env:MQTT_NODE_ID = "bench-node-1"
$env:FAN_INDEX_START = "1"
$env:FAN_COUNT = "2"
python -m pytest --run-live-mqtt
```

The standalone runner exposes the same scenarios:

```bash
python tools/stress_test.py --broker mqtts://broker.example:8883 \
  --node-id bench-node-1 \
  --fan-start 1 --fan-count 2 \
  --scenarios latency,spam,toggle,multi_mix,invalid,rmt_check
```

Both pytest teardown and the standalone runner make a best-effort request for
all configured fans to reach 0% and require fresh confirmations during normal
shutdown. The runner also inhibits that node's schedules for the rest of the
current boot so a schedule cannot immediately restart a fan. Loss of power,
Wi-Fi, MQTT, or the test process can prevent software cleanup; keep a physical
means of removing motor power available.

CI builds the non-secret fragments documented in [`configs/README.md`](configs/README.md):
a safety-oriented one-fan baseline, the four-RMT-channel maximum, reversible
optional features (including Home Assistant discovery), broker-restart policy,
legacy fan indexes 5–8, and the deployable production layer. Optional paths use
CI-only GPIOs and an `.invalid` HTTPS host. These builds exercise configuration
coverage; they neither provision credentials nor enable/burn Secure Boot or
flash-encryption eFuses.

## Operations and security references

- [`docs/production-deployment.md`](docs/production-deployment.md): build
  profiles, provisioning, MQTT authorization, signing, Secure Boot, encryption,
  rollback, canaries, and the partition decision record.
- [`docs/fleet-operations.md`](docs/fleet-operations.md): one-fan node scaling,
  onboarding, monitoring, outages, rotation, recovery, and decommissioning.
- [`docs/hardware-validation.md`](docs/hardware-validation.md): DShot600 capture
  analysis, startup-zero proof, interlock/tach checks, fault injection, and
  release evidence.
- [`docs/repository-security.md`](docs/repository-security.md): secret handling,
  credential rotation, coordinated history cleanup, and repository checks.
- [`docs/fault-response-policy.md`](docs/fault-response-policy.md): explicit
  deployment decisions for communication, task, tach, reset, persistence, and
  OTA failures.
- [`docs/ai-gardener.md`](docs/ai-gardener.md): an advisory-first camera/sensor
  companion architecture with executable observation/recommendation contracts
  and a deterministic authorization boundary.

Run the value-redacting repository check before pushing:

```bash
python scripts/check_repository_hygiene.py
```
