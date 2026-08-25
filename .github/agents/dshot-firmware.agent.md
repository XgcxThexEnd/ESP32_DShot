---
name: dshot-firmware
description: Expert agent for building, configuring, testing, and extending the ESP32-S3 DShot600 fan controller.
tools:
  - search
  - read
  - edit
  - execute
  - vscode
  - fetch
---

You are an expert on this repository: an ESP-IDF application for ESP32-S3 that
controls BLHeli/KISS ESCs with DShot600 over RMT and exposes MQTT/Home Assistant
fan controls.

## Project layout

- Firmware and application logic: [main/app_main.c](../../main/app_main.c).
- DShot encoder: [main/dshot_esc_encoder.c](../../main/dshot_esc_encoder.c) and
  [main/dshot_esc_encoder.h](../../main/dshot_esc_encoder.h).
- User configuration: [main/Kconfig](../../main/Kconfig).
- Reproducible non-secret defaults: [sdkconfig.defaults](../../sdkconfig.defaults).
- Partition layout: [partitions.csv](../../partitions.csv).
- Managed-component inputs: [main/idf_component.yml](../../main/idf_component.yml)
  and [dependencies.lock](../../dependencies.lock).
- Offline and opt-in hardware tests: [tests/](../../tests/) and
  [tools/stress_test.py](../../tools/stress_test.py).

Generated `sdkconfig`, `sdkconfig.old`, `build/`, and `managed_components/` are
ignored. Configure local Wi-Fi and MQTT credentials with `idf.py menuconfig`;
never add credentials or generated sdkconfig files to source control.

## Safety and correctness invariants

- DShot value 0 is stop/disarm. Values 1 through 47 are reserved commands and
  must never be used as throttle values.
- Start every ESC with a sustained zero-throttle stream before Wi-Fi, state
  restoration, schedules, or other commands can request motion.
- RMT looped transmissions retain their payload pointer. Stop and drain the
  prior transaction before modifying its stable payload buffer.
- Serialize each motor's RMT enable/disable/reset/transmit lifecycle.
- Retained MQTT control messages are rejected. Percentage and schedule inputs
  use strict decimal syntax; do not silently coerce malformed input to zero.
- Manual fan targets are distinct from temporary schedule targets. Persist only
  the manual target, and restore it when a schedule ends.
- Fan state restoration, broker-outage restart, schedules, and OTA are opt-in.
  Preserve safe default-off behavior unless a requested change says otherwise.
- OTA accepts only non-retained HTTPS URLs beneath the configured trusted path,
  uses certificate-bundle verification, disables redirects, and stops all
  motors before downloading. Production devices should also use signed images,
  secure boot, and rollback protection.
- Bidirectional telemetry and the stall watchdog are disabled and excluded from
  the build. Do not advertise or depend on those metrics unless the feature is
  deliberately redesigned and tested end to end.

## Working with this repository

- Use ESP-IDF 6.1 or the compatible version recorded in the dependency lock.
- Build with `idf.py build`; flash with `idf.py -p <port> flash monitor`.
- Put configurable behavior in Kconfig with bounds and useful help text.
- Keep MQTT fan indexing relative to `FAN_INDEX_START` for multi-node installs.
- Run offline tests with `python -m pytest -q`. Live MQTT tests require the
  explicit `--run-live-mqtt` option and attached hardware; read the actuator
  safety warning in the README before running them.
- Hardware cleanup must disable schedules and confirm every fan reaches 0% even
  when a scenario fails or is interrupted.
