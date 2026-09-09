# Hardware validation

This procedure validates the parts that host tests and an ESP-IDF build cannot:
the waveform at each configured GPIO, zero-throttle startup behavior, wiring,
interlock polarity, tachometer feedback, ESC behavior, and fault recovery. Run
it on every new board/ESC revision before putting a fan into service.

The CSV analyzer proves DShot600 timing, 16-bit frame boundaries, checksum,
non-use of reserved commands, and (with a suitable known vector) MSB-first wire
order. It does not prove that a motor is mechanically stopped, that the ESC
interprets a signal correctly, or that the output was safe before the first
captured edge.

## Safety and test setup

1. Remove the propeller or fan blade, or mechanically isolate the motor so an
   unexpected start cannot cause injury. Keep a physical means of removing ESC
   power within reach.
2. Initially leave the ESC signal and motor power disconnected. Power only the
   ESP32 and observe the configured DShot GPIO with a high-impedance logic
   analyzer or oscilloscope.
3. Connect analyzer ground to ESP32 ground and confirm the analyzer input is
   rated for the GPIO voltage. Never probe a motor phase or the ESC battery rail
   with a logic-analyzer input.
4. Use at least 50 MS/s when possible. DShot600's nominal bit period is
   1.667 us; its shortest nominal HIGH/LOW interval is about 0.417 us.
5. Record the firmware commit, ESP-IDF version, board revision, configured
   GPIOs, `FAN_COUNT`, `FAN_INDEX_START`, `MIN_SPIN_PCT`, enabled safety
   features, ESC model/firmware, instrument model, sample rate, and probe point.

Use an isolated, current-limited bench supply when ESC power is introduced.
Do not use a software command, MQTT disconnect, or test cleanup as the only
emergency stop.

## Capture format and analyzer

Export one DShot channel as a two-column CSV. Column one is a timestamp; column
two is the logic level *after* that transition. A single textual header and
comment lines beginning with `#` are allowed. Levels may be `0`/`1`,
`low`/`high`, or `false`/`true`.

```csv
time_s,level
0.000000000,1
0.000000625,0
0.000001667,1
```

The input must be an edge-transition export, not a uniformly sampled digital
trace. It must begin before a complete frame's first rising edge and end during
the LOW interval after a complete frame. Re-export a slightly shorter selection
if the tool reports a partial first or last frame.

Run the analyzer from the repository root:

```bash
python scripts/analyze_dshot_capture.py captures/fan1.csv \
  --time-unit s \
  --minimum-frame-gap-us 40
```

It exits zero only if every captured frame passes. The default 20% timing
tolerance accepts normal probe and sampling error while keeping the nominal
0-bit HIGH time (0.625 us) distinct from the nominal 1-bit HIGH time
(1.250 us). The firmware adds a 50 us post-frame LOW delay, so a 40 us minimum
is a useful project-specific acceptance threshold. Do not increase the timing
tolerance merely to make a failing capture pass; first inspect grounding,
sample rate, ringing, GPIO loading, and clock accuracy.

## Required commissioning checks

### 1. Boot zero-throttle stream

Keep the ESC signal disconnected. Trigger on the first rising edge after an
ESP32 reset and capture at least 1.6 seconds. The firmware deliberately emits a
repeating zero-throttle stream for 1500 ms before it can restore a target,
process a schedule, or connect to MQTT.

```bash
python scripts/analyze_dshot_capture.py captures/fan1-boot.csv \
  --time-unit s \
  --minimum-frame-gap-us 40 \
  --zero-arming-ms 1500 \
  --max-arming-frame-gap-us 100
```

Repeat this test on every DShot GPIO and after power-on reset, software reset,
and watchdog reset. The reported zero-frame count must cover the full interval,
there must be no nonzero or reserved command before the deadline, and there
must be no frame-start gap above 100 us. The analyzer measures the interval
from the first captured DShot frame; use a second oscilloscope channel on reset
or the hardware interlock if reset-to-first-edge behavior also needs evidence.

If fan-state restoration is enabled, make a separate mechanically safe capture
with a stored nonzero target. The transition to nonzero may occur only after
the zero-throttle interval and local boot-health checks. With restoration
disabled, a reboot must remain at zero until a fresh accepted command or an
enabled schedule.

### 2. Known-vector timing and wire order

Keep the motor unable to cause injury. Disable schedules/restoration for the
baseline, command a modest percentage, wait until `applied_percentage` is
steady, and capture several complete frames. Convert the applied percentage to
the expected DShot value using the same integer mapping as the firmware:

```text
effective_pct = max(requested_pct, MIN_SPIN_PCT)  # except 0 remains 0
expected_value = 48 + floor(effective_pct * 1999 / 100)
```

For example, an applied 20% maps to value 447. Validate the steady capture:

```bash
python scripts/analyze_dshot_capture.py captures/fan1-20pct.csv \
  --time-unit s \
  --minimum-frame-gap-us 40 \
  --expect-value 447 \
  --expect-telemetry 0
```

Use a nonzero asymmetric vector for this check. An all-zero frame looks the
same in either bit order and cannot prove MSB-first transmission; the analyzer
rejects any expected vector that is bit-order ambiguous. Captures taken while
the ramp is changing values should be checked without `--expect-value`, then
compared with MQTT requested/applied state and the configured ramp step.

Repeat on every channel. Command one fan at a time and confirm that only its
GPIO changes from zero. For a one-fan-per-ESP deployment, also confirm that each
node accepts only its node-scoped MQTT topic and credentials.

### 3. Interlock, ESC, and tachometer

After the logic-only checks pass, connect the ESC signal and then introduce ESC
power on the mechanically safe/current-limited rig.

- Confirm the optional hardware interlock remains inactive during boot and on
  every configured safety stop. Observe it on a separate instrument channel;
  the CSV analyzer evaluates only one DShot signal.
- Confirm zero throttle never starts the motor. Exercise a low, middle, and
  high test point appropriate for the rig and compare requested, applied, and
  measured behavior. Do not use an unrestricted full-throttle test merely for
  protocol validation.
- If tach feedback is enabled, verify pulses per revolution and RPM against an
  independent tachometer. Test the startup grace, debounce interval, alarm,
  exact `CLEAR` action, and configured stop-one/stop-all response. Simulate a
  missing sensor signal only while the motor can continue spinning safely;
  do not physically stall an energized motor unless the entire rig is rated
  for that test.
- Remove ESP32 power, ESC power, and signal in the orders expected in service.
  Confirm no back-powering, unintended start, or unsafe interlock state occurs.

### 4. Network and recovery faults

With the load still mechanically safe, retain serial logs and MQTT observations
while testing:

- malformed, empty, out-of-range, and retained commands;
- Wi-Fi loss, broker loss, reconnect, and the configured communication-lease
  action;
- reboot with schedules and restoration both disabled, then in each explicitly
  enabled configuration;
- repeated power cycles during zero, ramp-up, steady output, and ramp-down;
- NVS erase/migration reporting and a full NVS condition on a disposable test
  image;
- a valid OTA, loss of network during download, and forced rollback of a
  deliberately unhealthy pending image.

OTA, signing, Secure Boot, flash encryption, and eFuse exercises must follow
the production runbook and should begin on disposable hardware. A successful
download is not evidence that pending-image validation or rollback works.

### 5. Soak and multi-node checks

Run an instrumented soak test for the duration required by the deployment's
risk assessment. Cycle commands and planned network outages while recording
heap, task health, MQTT acknowledgements/outbox, RMT refresh/error counters,
tach alarms, resets, supply voltage, and ESC/motor temperature. A multi-node
rig should also verify duplicate-ID rejection in inventory, broker ACL isolation,
simultaneous reconnect load, and that stopping one node cannot address another.

## Evidence and acceptance record

Preserve the raw capture, analyzer command and complete output, serial log,
redacted MQTT trace, configuration fingerprint, and SHA-256 hashes of the
firmware binary and captures. Do not put credentials, private keys, or an
unredacted generated `sdkconfig` in the evidence bundle.

At minimum, release evidence should show:

| Check | Acceptance evidence |
| --- | --- |
| DShot timing | Every frame passes HIGH-width, bit-period, frame-gap, and checksum validation on every GPIO |
| Protocol safety | No values 1..47; known nonzero vector passes MSB-first and telemetry-bit check |
| Startup | At least 1500 ms of cadence-bounded zero frames from the first DShot edge |
| Channel isolation | Only the addressed fan/node leaves zero |
| Interlock | Safe polarity at reset and every injected safety fault |
| Tach | Independent RPM correlation and configured stall response |
| Network policy | Retained/malformed commands rejected; lease/reconnect behavior matches configuration |
| OTA/rollback | Healthy image confirms; unhealthy pending image rolls back and stays motion-safe |
| Soak | No unexplained reset, memory decline, stale task, missed safety action, or increasing RMT error trend |

Any analyzer failure is a failed hardware gate until explained and reproduced.
Archive exceptions with measured evidence and an explicit engineering decision;
do not normalize them by weakening the validator defaults.

## Machine-readable release evidence

Use [`hil/evidence-manifest.template.json`](../hil/evidence-manifest.template.json)
to make the required checks, hardware identity, test limits, and raw-artifact
hashes reviewable as one record. Copy the template into a release-specific
evidence directory and keep each artifact path relative to that copy. Actual
captures may be archived outside Git, but the manifest and its hashes must stay
with the release record.

The repository template is deliberately incomplete. Validate its structure, or
an in-progress copy, with:

```bash
python scripts/validate_hil_evidence.py \
  path/to/evidence/manifest.json --allow-incomplete
```

Before release, replace every placeholder, enter the actual supply/current/
temperature/soak limits, resolve every `not_run` result, set `record_status` to
`complete`, copy the tested bundle's complete flash package (including every
file named by `FLASH-MANIFEST.json`), redacted sdkconfig, and
`RELEASE-METADATA.json` to the recorded relative paths, enter their hashes and
the node-reported topology fingerprint, and run the strict gate:

```bash
python scripts/validate_hil_evidence.py path/to/evidence/manifest.json
```

Strict validation verifies required check IDs, configuration/hardware
consistency, non-overlapping GPIO assignments, safe relative artifact paths,
file presence, and SHA-256 integrity. It enforces the required artifact IDs,
kinds, distinct paths, and check-to-artifact mappings documented in
[`hil/README.md`](../hil/README.md). It also verifies the schema-v1 flash plan
and every packaged bootloader, partition, OTA-data, and application file; the
resolved sdkconfig must be parseable and contain no populated sensitive value.
It permits `not_applicable` only for the interlock, tach, and OTA/rollback
checks and requires a written justification; an enabled interlock or tachometer
cannot waive its corresponding check. The validator does not interpret the
captured electrical or thermal measurements, so an engineer must still compare
every result with the recorded acceptance limits and this procedure.
