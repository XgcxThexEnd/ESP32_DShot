# ESP32 DShot Firmware — Code Review Report

**Project:** ESP32-S3 DShot ESC Controller  
**Reviewer:** Automated static review  
**Date:** 2026-08-25  
**Scope:** All C source and header files in `main/`, build config files, and host-side tests in `tests/c/`

---

## 1. Architecture & Separation of Concerns

The codebase exhibits a well-disciplined modular architecture refactored from a monolithic prototype.

**Strengths:**

- **Composition root in `app_main.c`** wires modules via callback config structs (dependency injection). Each module receives its dependencies through a typed `*_config_t` struct with function pointers and a `callback_context`. See `fan_state_store_config_t` (`fan_state_store.h:20-25`), `scheduler_config_t` (`scheduler.h:70-75`), `safety_supervisor_callbacks_t` (`safety_supervisor.h:54-60`).

- **Side-effect-free parser**: `command_router.c` is a pure function — `command_router_parse()` (`command_router.c:245-307`) validates and parses MQTT messages without performing any I/O or motor actions. The caller in `app_main.c:618-627` applies the result. This makes the router independently testable, as demonstrated by `tests/c/test_command_router.c`.

- **Policy extraction**: `controller_policy.c` isolates reusable validation logic (identifier validation, URL allowlisting, DShot mapping) from both the command router and the motor controller.

- **Clean header contracts**: Every header documents preconditions, ownership semantics, and thread-safety. See `motor_control.h:60-66` (callback contract), `mqtt_manager.h:26-36` (dispatch callback contract), `fan_state_store.h:51-63` (sync save semantics).

- **Feature-gated compilation**: Optional features (tach, scheduling, OTA, legacy topics, restore) are `#ifdef`-guarded with stub implementations, preventing link errors when disabled. See `scheduler.c:1052-1173`, `ota_manager.c:264-295`.

- **Single responsibility per module**: Each `.c` file owns one subsystem. No cross-module global variable access except the immutable `app_config_t` singleton.

| Finding | Severity | Location |
|---------|----------|----------|
| `app_main.c` is 1358 lines and acts as composition root, callback dispatcher, tach-action coordinator, and OTA coordinator. Consider extracting the tach-action task logic (lines 214–424) and MQTT command dispatch (lines 598–773) into separate coordinator modules. | Low | `app_main.c:214-424, 598-773` |

---

## 2. Code Quality (Naming, Error Handling, Defensive Programming, const/static)

**Strengths:**

- **Consistent `static` usage**: All file-scope variables and internal functions are `static`. No external linkage leakage. Verified across all modules.

- **`const` correctness**: `app_config_get()` returns `const app_config_t *` (`app_config.h:53`). The command router config is passed as `const` (`command_router.h:82`). Motor snapshots are filled by value. Consumer modules receive const pointers.

- **Defensive null checks**: Nearly every public function validates its pointer arguments. See `command_router_parse` (`command_router.c:258`), `motor_control_init` (`motor_control.c:721`), `tach_monitor_init` (`tach_monitor.c:322-325`).

- **Error propagation**: ESP-IDF `esp_err_t` is used consistently. `retain_first_error()` (`motor_control.c:318-323`) preserves the first failure during multi-step operations. `preserve_first_error()` (`safety_supervisor.c:133-136`) does the same for safety stops.

- **Named result enums**: `command_router_result_t` (`command_router.h:14-24`) and `ota_manager_start_result_t` (`ota_manager.h:44-55`) provide granular failure reasons.

- **`_Static_assert` for persistent layouts**: `fan_state_store.c:51-56` asserts blob sizes and checksum offsets. `scheduler.c:61-62` asserts entry layout size.

| Finding | Severity | Location |
|---------|----------|----------|
| `dshot_esc_encoder.c:36` casts `primary_data` to `dshot_esc_throttle_t*` without a null check. The RMT framework guarantees non-NULL in practice, but the encoder's own contract doesn't document this. | Low | `dshot_esc_encoder.c:36` |
| `app_config.c:213` — `snprintf` return value not checked for the MAC-based node ID. The buffer (49 bytes) is always sufficient for the format, but the unchecked return is inconsistent with the project's otherwise thorough checking. | Info | `app_config.c:213` |
| Variable naming: `s_rx` in `mqtt_manager.c:116` is a single static assembly buffer. The name is clear in context but a more descriptive name like `s_rx_assembly` would aid readability. | Info | `mqtt_manager.c:116` |

---

## 3. Safety & Robustness (Fault Detection, Watchdog, Failsafes, Hardware Interlocks, Zero-Throttle Arming)

This is the strongest dimension of the codebase. Safety is designed in from the first line of `app_main()`.

**Strengths:**

- **Hardware interlock first**: `app_main.c:1022` — `safety_supervisor_init()` is the first operation, asserting the independent hardware inhibit before any configuration or driver initialization. The interlock GPIO is set to its inactive level *before* the pad direction is changed (`safety_supervisor.c:329-331`), preventing an active-low relay pulse during boot.

- **Zero-throttle arming**: `app_main.c:1146` — After RMT initialization, a 1500ms zero-throttle stream (`MOTOR_BOOT_SETTLE_MS`) is sent before any target, schedule, Wi-Fi, or MQTT operation. ESCs see idle frames before arming.

- **Multi-layered failsafe stops**: `safety_supervisor.c:151-309` implements a phased stop: (1) deassert interlock, (2) latch motor inhibit, (3) inhibit automatic sources (scheduler), (4) wait for acknowledged NVS commit, (5) escalate temporary stops to global latch on any failure. Each phase feeds the watchdog (`feed_supervisor_watchdog_if_current`).

- **RMT fault detection**: `motor_control.c:179-209` — consecutive RMT errors are counted. After `CONFIG_RMT_ERROR_LIMIT` (default 5) consecutive failures, the motor is latched off and the supervisor is notified. `CONFIG_RMT_FAILURE_REBOOT` can trigger a reboot to reset stuck RMT hardware.

- **Tach stall detection**: `tach_monitor.c:202-295` — RPM is sampled via PCNT. Stall (RPM below threshold) and sensor-invalid conditions are debounced before latching. A startup grace period (`CONFIG_TACH_STARTUP_GRACE_MS`) prevents false alarms during spin-up. The confirmed-stall callback dispatches to a dedicated action task (`app_main.c:301-393`) that serializes safety stops.

- **OTA rollback with local health window**: `app_main.c:831-953` — A pending OTA image must pass a local health window where motor ramp, supervisor, MQTT dispatch, scheduler, and tach task heartbeats are all proven advancing, and all motors hold safe zero. Failure triggers `esp_ota_mark_app_invalid_rollback_and_reboot()`.

- **Reset reason gating**: `app_main.c:108-116` — Only POWERON, SW, and DEEPSLEEP resets allow motion state restore. Panic/watchdog/brownout resets commit zero targets and inhibit schedules, preventing a failed-safe condition from being re-authorized.

- **Communication lease**: `safety_supervisor.c:621-638` — If MQTT broker ACKs are not received within `CONFIG_COMMUNICATION_LEASE_MS`, motors are latched off via a communication inhibit. A fresh manual command clears this latch.

- **MQTT command queue overflow**: `mqtt_manager.c:268-288` — Queue saturation forces a fail-closed disconnect (all commands from the session are invalidated). The supervisor detects the overflow and latches communication off (`safety_supervisor.c:602-619`).

- **Task watchdog**: The supervisor task (`safety_supervisor.c:440-649`) and ramp task (`motor_control.c:598-717`) register with the ESP task watchdog. The supervisor monitors heartbeats of all critical tasks and triggers fail-safe restarts.

| Finding | Severity | Location |
|---------|----------|----------|
| **OTA URL prefix validation not blocking**: `app_main.c:807-810` — `network_config_valid()` logs an error when `ota_manager_url_allowed(CONFIG_OTA_ALLOWED_URL_PREFIX)` returns false, but does not set `valid = false`. This means network services start normally with OTA non-functional. While the comment says "OTA requests will be rejected" (true at runtime), this is inconsistent with the credential/URI checks that do set `valid = false`. An operator might miss the log message. | Low | `app_main.c:807-810` |
| **Supervisor restart retry throttling**: `safety_supervisor.c:412-423` — `restart_attempt_due()` throttles restart attempts to 5-second intervals. If a fail-safe stop fails (e.g., NVS commit timeout), the supervisor waits 5 seconds before retrying. During this window, motors could remain in an unsafe state if the interlock also failed. The design assumes the interlock provides the primary hardware safety. | Low | `safety_supervisor.c:412-423` |
| **`startup_failure_safe` infinite loop on rollback failure**: `app_main.c:980, 993` — If OTA rollback fails, the code enters `for (;;) vTaskDelay(pdMS_TO_TICKS(1000))`. This is intentional (remain inhibited for service), but the device is effectively bricked without a power cycle or external recovery. No watchdog reboot is triggered. | Info | `app_main.c:980, 993` |

---

## 4. Concurrency (FreeRTOS Task Sync, Mutex/Critical Sections, Race Conditions, Lock Ordering)

**Strengths:**

- **Documented lock ordering**: `motor_control.c:1120-1122` — Comment documents that `apply_motor` owns `io_mutex` before `s_motor_lock`, and `holds_safe_zero` acquires all I/O mutexes in fan order for a coherent proof.

- **`portMUX_TYPE` for brief state**: All modules use spinlock critical sections (`portENTER_CRITICAL`/`portEXIT_CRITICAL`) for short state reads/writes (flags, counters, generations). See `motor_control.c:83-84`, `mqtt_manager.c:74`, `fan_state_store.c:58`.

- **FreeRTOS mutexes for long operations**: `io_mutex` per motor (`motor_control.c:38`) serializes RMT operations. `s_stop_mutex` (recursive) in safety supervisor (`safety_supervisor.c:58`) serializes stop/enable transactions. `s_lifecycle_mutex` in MQTT manager (`mqtt_manager.c:85`) serializes client lifecycle.

- **Generation counters**: Connection generation (`mqtt_manager.c:101`), control generation (`motor_control.c:51`), and fault generation (`tach_monitor.c:35`) detect stale operations. The MQTT command commit fence (`mqtt_manager.c:728-750`) revalidates the session generation under the commit mutex.

- **Recursive mutex for safety**: `safety_supervisor.c:343` — `xSemaphoreCreateRecursiveMutex()` allows the stop transaction to be re-entered from the same task (e.g., fallback global stop from within a maintenance stop).

- **Callbacks outside critical sections**: `motor_control.c:146-165` — `emit_event` copies state under the lock, releases the lock, then invokes the callback. `tach_monitor.c:275-294` — `sample_fan` latches state under the lock, releases, then calls `confirmed_stall`. This prevents callback deadlocks.

- **Stale-command rejection**: `mqtt_manager.c:189-201` — The dispatch task checks `connection_generation` before invoking the callback. Stale commands (from a disconnected session) are dropped and counted.

| Finding | Severity | Location |
|---------|----------|----------|
| **`s_dispatch_scratch` shared static without lock**: `mqtt_manager.c:119` — `s_dispatch_scratch` is a static `mqtt_dispatch_message_t` written in `mqtt_manager_enqueue_assembled_message` (line 257-266) and then copied into the queue. ESP-MQTT serializes DATA events on its event task, so only one event task accesses this at a time. However, this assumption is not documented in the code, and a future refactor could break it. | Low | `mqtt_manager.c:119, 257-266` |
| **Scheduler `reserve_fan_operation` busy-wait**: `scheduler.c:148-164` — Uses a polling loop with `vTaskDelay(1)` and a 1-second timeout. This works but wastes CPU compared to a semaphore or queue-based reservation. Acceptable given the low-frequency nature of schedule operations. | Low | `scheduler.c:148-164` |
| **`fan_state_store_save_sync` semaphore consumption**: `fan_state_store.c:303-323` — The completion-wait loop uses a binary semaphore. If the save task completes two saves before the waiter checks, the second `xSemaphoreGive` is lost (binary semaphore can only hold one count). The generation check at the top of each loop iteration catches this, but the pattern relies on the generation check rather than the semaphore count. This is correct but subtle. | Info | `fan_state_store.c:303-323` |

---

## 5. Security (Input Validation, Credential Handling, Transport Security, OTA Security)

**Strengths:**

- **Credentials from Kconfig**: `app_main.c:41-58` — Wi-Fi SSID/password and MQTT broker URI/username/password come from menuconfig, keeping secrets out of source control. Compile-time `#ifndef` fallbacks are empty strings.

- **Production policy gates**: `Kconfig:244-249` (`WIFI_REQUIRE_SECURE_AUTH`) rejects open/WPA1 APs. `Kconfig:319-325` (`MQTT_REQUIRE_TLS_AUTH`) requires `mqtts://` plus non-empty credentials. `app_main.c:793-805` enforces these at runtime.

- **OTA URL allowlisting**: `controller_policy.c:87-118` — `controller_https_url_allowed` enforces: (1) `https://` scheme, (2) trusted prefix with trailing `/`, (3) no control characters or backslashes, (4) no percent-encoding, (5) no `.` or `..` path segments. This prevents path traversal and scheme confusion.

- **OTA redirects disabled**: `ota_manager.c:236` — `disable_auto_redirect = true` prevents redirect-based URL bypass.

- **TLS certificate bundle**: `mqtt_manager.c:866` and `ota_manager.c:237` — Both attach `esp_crt_bundle_attach` for CA verification.

- **TLS time validation**: `mqtt_manager.c:138-144` and `wifi_manager.c:440-444` — Wall time must be plausible (≥ 2024-01-01) before TLS connections, preventing certificate date bypass via reset-time epoch. OTA is rejected until time is trusted (`ota_manager.c:132-135`).

- **Retained command rejection**: `command_router.c:272` — All retained MQTT commands are rejected, preventing replay of stale motor commands after reconnection.

- **Embedded NUL rejection**: `command_router.c:267-270` — Topics and payloads with embedded NUL bytes are rejected.

- **PMF (Protected Management Frames)**: `wifi_manager.c:350-355` — PMF is configurable and required by default (`Kconfig:251-256`).

- **Input size bounding**: `command_router.c:263-266` — Topics and payloads exceeding fixed capacities are rejected before parsing. `mqtt_manager.c:467-468` rejects oversized MQTT messages.

| Finding | Severity | Location |
|---------|----------|----------|
| **`mqtt://` (plaintext) allowed by default**: While `Kconfig:319-325` provides a production policy gate (`MQTT_REQUIRE_TLS_AUTH`, default y), the `mqtt_manager.c:865-870` code path for `mqtt://` only logs a warning. If the policy gate is disabled for development, all MQTT traffic (including motor commands) is unencrypted. The warning is appropriate, but the system allows it. | Low | `mqtt_manager.c:865-870` |
| **`strlcpy` truncation of `mqtt_root_topic`**: `app_config.c:222` — `strlcpy(config->mqtt_root_topic, CONFIG_MQTT_ROOT_TOPIC, sizeof(config->mqtt_root_topic))` copies into a 128-byte buffer. `controller_topic_level_valid` (`app_config.c:203`) validates characters but **not length** against the buffer size. A `CONFIG_MQTT_ROOT_TOPIC` longer than 127 characters would be silently truncated, producing a different MQTT topic prefix than configured. The `strlcpy` return value is not checked. | Medium | `app_config.c:203, 222` |
| **WiFi credentials in firmware binary**: Kconfig string values are compiled into the binary. This is standard for ESP-IDF but means credentials are extractable from the firmware image. No NVS-based provisioning is provided. | Info | `app_config.c:23-31`, `main/Kconfig:232-242` |

---

## 6. Potential Bugs (Buffer Overflows, Integer Overflow, Memory Leaks, Race Conditions, Edge Cases)

**Strengths:**

- **No buffer overflows found**: All `memcpy`/`snprintf` operations are bounds-checked against named capacities. `command_router.c:276-279` copies topic/payload into fixed stack buffers after length validation.

- **Integer overflow protection**: `controller_policy.c:43-47` — `controller_parse_uint_field` checks `value > limit / 10UL` before multiplying. `command_router.c:21` — `core_config_valid` checks `fan_index_start > INT_MAX - (fan_count - 1)`.

- **RPM calculation overflow-safe**: `tach_monitor.c:209-213` — Uses `uint64_t` for the numerator and caps the result at `UINT32_MAX`.

- **Schedule interval arithmetic**: `scheduler.c:244` — `(int64_t)persisted->interval_min * 60 * 1000000` casts to 64-bit before multiplication. Max value (525600 × 60 × 1,000,000 = 3.15 × 10^13) fits in `int64_t`.

- **Resource cleanup on failure**: `motor_control.c:414-452` — `fail_motor_initialization` transactionally unwinds all allocated RMT resources. `wifi_manager.c:61-115` — `fail_initialization` unwinds all WiFi/SNTP/event resources. `tach_monitor.c:76-124` — `fail_initialization` releases PCNT resources.

- **Memory management**: `ota_manager.c:139-144` — URL is `malloc`'d and freed on all error paths. `ota_manager.c:250-258` — The OTA task frees the URL on both success and failure paths.

| Finding | Severity | Location |
|---------|----------|----------|
| **`state_publisher.c:73` legacy publish short-circuit**: `published = mqtt_manager_publish(topic, payload, qos, retain) && published;` — If the node-topic publish succeeds but the legacy publish fails, `published` becomes `false`. The caller (`publish_one_state`) discards the return value, so this is cosmetic. But `publish_fan_leaf`'s contract implies "at least one publish succeeded" when returning `true`, and this logic breaks that contract for the legacy-mirroring case. | Low | `state_publisher.c:73` |
| **`mqtt_manager_publish` blocks with `portMAX_DELAY`**: `mqtt_manager.c:929` — `xSemaphoreTake(lifecycle_mutex, portMAX_DELAY)` means publish calls block indefinitely if `mqtt_manager_poll` or `mqtt_manager_start` holds the lifecycle mutex (e.g., during the 100ms offline LWT publish delay at line 988). State publisher calls could be starved. Not safety-critical (supervisor doesn't publish). | Low | `mqtt_manager.c:929, 988` |
| **`dshot_esc_encoder.c` fall-through without default**: The `switch` at line 43 has no `default` case. If `dshot_encoder->state` is neither 0 nor 1 (memory corruption), the function falls through without encoding anything. The RMT framework would see zero symbols. This is fail-safe (no motor movement) but silent. | Low | `dshot_esc_encoder.c:43-65` |
| **`app_main.c:603` `strlen(data)` on MQTT payload**: `if (!topic || !data || !session || len != strlen(data))` — `strlen` scans the payload. The MQTT manager already rejects embedded NULs, so this is defense-in-depth, but `strlen` on a large (768-byte) payload is O(n) on every command. | Info | `app_main.c:603` |

---

## 7. Magic Numbers vs Named Constants

**Strengths:**

- Most timing constants are named via `#define` at file scope. See `motor_control.c:24-32`, `safety_supervisor.c:25-32`, `mqtt_manager.c:38-43`, `app_main.c:97-106`.

- Kconfig exposes all user-tunable values with documented ranges. See `main/Kconfig` for `RAMP_STEP_PCT` (range 1-20), `RAMP_TICK_MS` (range 10-100), `SUPERVISOR_TASK_WDT_MS` (range 6000-30000), etc.

- `command_router.c:8` — `MAX_SCHEDULE_MINUTES` is a named constant for 525600 (minutes per year).

- `app_config.h:14-16` — `APP_CONFIG_MAX_FANS`, `APP_CONFIG_NODE_ID_CAPACITY`, `APP_CONFIG_TOPIC_CAPACITY` are named capacities.

| Finding | Severity | Location |
|---------|----------|----------|
| **Boot health window magic numbers**: `app_main.c:834` (`1250`), `:841` (`2750`), `:845-846` (`750`), `:928` (`100`), `:930` (`750`), `:934` (`1500`) — The pending-image health window uses several unexplained numeric literals for minimum durations. These appear to be derived from task periods but are not named or documented. | Low | `app_main.c:834-946` |
| **DShot duty cycle constants**: `dshot_esc_encoder.c:105-108` — `7485`, `37425`, `10000`, `100000` are DShot600 duty cycle percentages scaled by 10000. A comment explains "74.850% and 37.425%" but the raw numbers could use named constants. | Low | `dshot_esc_encoder.c:105-108` |
| **TLS time threshold**: `mqtt_manager.c:143` and `wifi_manager.c:443` — `1704067200` (2024-01-01T00:00:00Z) is duplicated. A shared named constant would prevent divergence. | Low | `mqtt_manager.c:143`, `wifi_manager.c:443` |
| **MQTT session/network constants**: `mqtt_manager.c:853-855` — `keepalive = 30`, `reconnect_timeout_ms = 3000`, `timeout_ms = 5000` are unnamed literals in the client config struct. | Info | `mqtt_manager.c:853-855` |
| **WiFi reconnect backoff**: `wifi_manager.c:144` — `shift = retry_count > 16 ? 16 : retry_count` and `:147` — `delay_ms / 4U` for jitter window. These are reasonable but undocumented. | Info | `wifi_manager.c:144-147` |
| **State publisher buffer sizes**: `state_publisher.c:92-114` — `discovery[160]`, `unique_id[96]`, `payload[896]`, `node_info[512]`, `fans[48]`, `announce[144]`, `sensor_payload[640]` are sized by inspection. Comments documenting the derivation would help maintenance. | Info | `state_publisher.c:92-114, 178, 225, 240` |

---

## 8. snprintf / strlcpy Return Value Checking

**Strengths:**

- **Checked returns in critical paths**: 
  - `app_config.c:321-324` — `vsnprintf` and `snprintf` returns checked for topic formatting.
  - `mqtt_manager.c:835-837` — `snprintf` return checked for client ID.
  - `state_publisher.c:115-127, 179-219, 241-246, 366-415` — All major JSON payloads check `snprintf` return against buffer size, logging an error on overflow.
  - `controller_policy.c:29-30` — `snprintf` return checked for node topic.

- **`(void)snprintf` for safe small formats**: `state_publisher.c:262-272` — Value formatting into a 24-byte buffer for `uint8_t`/`uint32_t` values (max 10 chars) is safe by construction. The `(void)` cast documents the intentional skip.

| Finding | Severity | Location |
|---------|----------|----------|
| **`safety_supervisor.c:160` — `snprintf` return cast to void**: `(void)snprintf(reason_copy, sizeof(reason_copy), "%s", safe_reason)` — Truncation is intentional (the full reason is logged separately), but the return value could indicate truncation for diagnostic purposes. The `reason_copy` is only used for the stored `s_last_stop_reason` field. | Info | `safety_supervisor.c:160` |
| **`app_main.c:492-494` — `snprintf` return cast to void**: `(void)snprintf(out_snapshot->boot_health, ...)` — The boot health name is bounded by the enum (max "rollback_failed_safe" = 21 chars) and the buffer is 32 bytes. Safe in practice. | Info | `app_main.c:492-494` |
| **`state_publisher.c:226-237` — incremental `snprintf` in fans list**: `position += snprintf(fans + position, ...)` — The return value of each incremental `snprintf` is not checked for negativity (though `snprintf` only returns negative on encoding errors, which won't occur with `%d` format). The loop condition `position > 0 && position < (int)sizeof(fans)` guards against overflow. With max fan numbers (1-11), the 48-byte buffer is always sufficient. | Low | `state_publisher.c:226-237` |

---

## Strengths Summary

1. **Safety-first boot order**: Interlock asserted → config validated → zero-throttle RMT stream → 1500ms arming delay → tasks started → health window verified → interlock enabled → network started.

2. **Transactional safety stops**: Multi-phase stop with interlock deassert, motor inhibit, automatic-source inhibit, NVS commit, and escalation to global latch on any phase failure.

3. **OTA rollback with local self-test**: Pending image must prove all critical task heartbeats are advancing and motors hold zero before `mark_app_valid_cancel_rollback`.

4. **Pure command parser**: `command_router_parse` is side-effect-free, enabling comprehensive host-side testing without ESP-IDF hardware mocks.

5. **Comprehensive input validation**: NUL-byte rejection, size bounding, integer overflow protection, path traversal prevention, retained-message rejection, URL allowlisting.

6. **Consistent concurrency discipline**: Spinlocks for brief state, mutexes for long operations, callbacks outside critical sections, generation counters for stale detection, documented lock ordering.

7. **Persistent state integrity**: FNV-1a checksums, topology fingerprints, schema versioning, static asserts on layout sizes, all-zero-only legacy migration.

8. **Watchdog integration**: Both supervisor and ramp tasks register with ESP task watchdog. Supervisor feeds watchdog between stop phases.

9. **Test coverage**: Host-side tests exercise the command router (all command types, edge cases, malformed input) and controller policy (identifiers, topics, percentages, DShot frames, URL validation). Fuzz harness covers the policy layer.

10. **Clean build configuration**: Kconfig exposes all tunable parameters with documented ranges and helpful text. CMakeLists.txt declares all dependencies explicitly. `idf_component.yml` pins the MQTT component version.

---

## Findings Summary Table

| Severity | Count | Key Findings |
|----------|-------|--------------|
| **Critical** | 0 | — |
| **High** | 0 | — |
| **Medium** | 1 | `strlcpy` truncation of `mqtt_root_topic` without length validation (`app_config.c:203, 222`) |
| **Low** | 12 | OTA URL prefix non-blocking; supervisor restart throttling; `s_dispatch_scratch` undocumented serialization assumption; scheduler busy-wait; legacy publish short-circuit; `mqtt_manager_publish` blocking; DShot encoder missing default case; boot health window magic numbers; DShot duty cycle literals; duplicated TLS time threshold; MQTT session literals; `app_main.c` size |
| **Info** | 10 | `snprintf` void casts; `strlen` on payload; `app_main.c` infinite loops on rollback failure; semaphore consumption pattern; WiFi credentials in binary; MQTT plaintext allowed; `dshot_esc_encoder` null assumption; naming; state publisher buffer sizes; incremental `snprintf` |

**Overall Assessment:** This is a high-quality embedded firmware codebase with exemplary safety engineering. No Critical or High severity issues were found. The one Medium finding (silent `strlcpy` truncation of `mqtt_root_topic`) should be addressed by adding a length check. The Low findings are mostly style/robustness improvements that do not present immediate safety risks. The architecture's layered failsafes — hardware interlock, zero-throttle arming, RMT fault latching, tach stall detection, communication lease, watchdog, and OTA rollback — provide defense in depth appropriate for a system controlling physical motors.