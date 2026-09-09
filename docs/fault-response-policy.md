# Fault-response policy

There is no universally safe fan response. Continued rotation can endanger a
person performing maintenance, while stopping greenhouse ventilation can damage
plants or equipment. Approve one response for each deployment class before its
resolved `sdkconfig` is accepted for release.

Record the decision, rationale, maximum detection/response time, required
physical protection, and HIL evidence for every row:

| Fault | Firmware choices today | Deployment decision required |
| --- | --- | --- |
| MQTT/broker acknowledgement loss | continue local output/schedules, or latch all motors off after a lease | whether ventilation availability or remote-stop expectation dominates; lease duration if stopping |
| Wi-Fi, DNS, SNTP, or TLS outage | operate locally while network services recover | whether local schedules/restoration are allowed and how operators detect isolation |
| Repeated RMT failure | latch output inhibited and normally reboot after safe-state persistence | maximum acceptable fault-to-interlock and fault-to-physical-stop time |
| Ramp/scheduler/tach/MQTT-dispatch task stall | supervisor attempts a durable global stop and restart | external watchdog/interlock behavior if the MCU or a mutex cannot make progress |
| Tach stall or invalid feedback | alarm only, stop the affected fan, or stop all fans | calibrated RPM threshold, startup grace, debounce, redundancy, and locked-rotor withstand time |
| Brownout, panic, or watchdog reset | suppress nonzero restoration and persist zero/schedule inhibit | supply integrity, restart policy, and whether loss of all ventilation needs independent redundancy |
| Planned reboot or power-on | optional target restoration after the zero-arming period | whether unattended motion restoration is allowed |
| NVS initialization/version failure | erase and recreate selected state, then report it | recovery authority and whether the unit stays physically isolated until configuration is reconciled |
| OTA preparation/download/first boot | inhibit motion and schedules; validate or roll back locally | signing/replay policy, canary cohort, power-loss recovery, and post-boot evidence |
| Gardener camera/model unavailable or uncertain | no model-originated change | observation freshness, quality/confidence threshold, and human escalation |

The current communication policy supports **continue** or **stop after lease**.
If the hazard analysis selects a known fallback ventilation percentage, that is
a new controller feature: specify its bounds, duration, interaction with tach
faults, persistence behavior, and manual override before implementing it.

## Hardware assumptions to record

- The motor power disconnect or contactor is rated for the actual fault current.
- The enable circuit is externally biased to its safe state before `app_main()`
  executes and whenever the ESP32 pin is high impedance.
- A hardwired emergency stop does not depend on MQTT, the ESP32, or the gardener.
- Supply, wiring, protection, enclosure, and grounding are approved for the
  installation's voltage, current, cable length, humidity, and temperature.
- Tach feedback proves shaft motion only. If airflow is safety-relevant, an
  appropriate airflow, pressure, temperature, or redundant sensor closes that
  hazard separately.

## Configuration gate

`scripts/check_production_sdkconfig.py` turns the reviewed choices into
policy-as-code. Ordinary CI validates its credential-free production template:

```bash
python scripts/check_production_sdkconfig.py \
  --sdkconfig build-production/sdkconfig --mode template
```

For a provisioned release, copy and review
`configs/production-release-policy.example.json`, then compare that policy with
the exact resolved configuration:

```bash
python scripts/check_production_sdkconfig.py \
  --sdkconfig sdkconfig.production.local \
  --mode release \
  --policy path/to/reviewed-device-class-policy.json
```

The example is deliberately not an approved wiring profile. Its GPIOs,
interlock polarity, tach calibration, communication behavior, and irreversible
security settings must be replaced with decisions backed by the actual hardware
and recovery plan. Diagnostics print symbol names and failure categories, never
credential values.
