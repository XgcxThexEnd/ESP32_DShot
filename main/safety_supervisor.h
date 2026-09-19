#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SAFETY_SUPERVISOR_REASON_CAPACITY 80

typedef enum {
    SAFETY_BOOT_INITIALIZING = 0,
    SAFETY_BOOT_HEALTHY,
    SAFETY_BOOT_HEALTHY_OFFLINE_CONFIG,
    SAFETY_BOOT_HEALTHY_OFFLINE_WIFI,
    SAFETY_BOOT_FAILED_SAFE,
    SAFETY_BOOT_ROLLBACK_FAILED_SAFE,
} safety_boot_health_t;

/**
 * Invoked only after the independent interlock and either a reversible
 * maintenance inhibit or a durable motor inhibit are already asserted. The
 * implementation must honor timeout_ticks. Return ESP_OK only after the
 * automatic-source inhibit is committed; an error makes a plain/OTA stop fall
 * back to a durable global motor inhibit.
 */
typedef esp_err_t (*safety_supervisor_inhibit_automatic_fn)(
    void *context, TickType_t timeout_ticks);

/** Commit cleared manual targets before a safety stop can report success. */
typedef esp_err_t (*safety_supervisor_request_persistence_fn)(
    void *context, TickType_t timeout_ticks);

/** Invoked after a stop attempt so a normal task can coalesce state output. */
typedef void (*safety_supervisor_notify_state_fn)(void *context);

/** Coordinator-owned tach safety-action worker state. */
typedef struct {
    bool ready;
    int64_t heartbeat_us;
    /** Oldest queued or in-flight confirmed-fault action; zero when idle. */
    int64_t oldest_action_since_us;
    uint32_t stack_bytes;
} safety_supervisor_tach_action_health_t;

typedef bool (*safety_supervisor_read_tach_action_health_fn)(
    void *context,
    safety_supervisor_tach_action_health_t *out_health);

typedef struct {
    safety_supervisor_inhibit_automatic_fn inhibit_automatic;
    safety_supervisor_request_persistence_fn request_persistence;
    safety_supervisor_notify_state_fn notify_state;
    safety_supervisor_read_tach_action_health_fn read_tach_action_health;
    void *callback_context;
} safety_supervisor_callbacks_t;

typedef struct {
    bool initialized;
    bool hardware_interlock_present;
    bool interlock_enabled;
    bool supervisor_started;
    bool watchdog_configured;
    bool stop_in_progress;
    bool global_safety_latched;
    bool communication_failsafe_active;
    bool rmt_inhibited;
    bool maintenance_inhibited;
    bool ramp_liveness_fault;
    bool scheduler_liveness_fault;
    bool tach_liveness_fault;
    bool tach_action_ready;
    bool tach_action_pending;
    int64_t tach_action_age_ms;
    uint32_t tach_action_stack_bytes;
    bool mqtt_command_overflow_fault;
    bool mqtt_dispatch_liveness_fault;
    safety_boot_health_t boot_health;
    int64_t heartbeat_us;
    uint32_t stack_bytes;
    uint32_t stop_count;
    esp_err_t last_stop_result;
    char last_stop_reason[SAFETY_SUPERVISOR_REASON_CAPACITY];
} safety_supervisor_health_t;

/**
 * Configure and assert the independent hardware interlock OFF.
 *
 * This API is deliberately configuration-independent and is intended to be
 * the first action in app_main(), before app configuration, NVS, RMT, or any
 * task is initialized. Repeated successful calls are harmless.
 */
esp_err_t safety_supervisor_init(void);

/** Install optional, bounded callbacks before safety_supervisor_start(). */
esp_err_t safety_supervisor_configure(
    const safety_supervisor_callbacks_t *callbacks);

/** Configure the ESP task watchdog when CONFIG_SUPERVISOR_ENABLED is set. */
esp_err_t safety_supervisor_configure_watchdog(void);

/** Start RMT-fault, ramp-heartbeat, and communication-lease supervision. */
esp_err_t safety_supervisor_start(void);

/**
 * Enable the independent motor output only when motor_control reports that no
 * inhibit is active. A generation check prevents this operation from
 * overtaking a concurrent safety stop.
 */
bool safety_supervisor_enable_output_if_safe(void);

/**
 * Serialize a manual output mutation with the complete multi-phase safety-stop
 * transaction. A successful begin must be paired with
 * safety_supervisor_output_command_end() on the same task. The guard may be
 * held while motor/scheduler state is changed, but not while publishing MQTT.
 */
esp_err_t safety_supervisor_output_command_begin(TickType_t timeout_ticks);
void safety_supervisor_output_command_end(void);

/**
 * Deassert the interlock first, inhibit automatic sources, and synchronously
 * stop every initialized motor. latch_global makes the motor inhibit durable
 * until reboot; clear_manual_targets also requests persisted-state update.
 */
esp_err_t safety_supervisor_stop_all(const char *reason,
                                     bool clear_manual_targets,
                                     bool latch_global);

/**
 * Latch every fan off through its individually clearable tach inhibit. This is
 * durable against manual commands until the tach alarms are cleared, without
 * escalating the event to the reboot-only global safety latch.
 */
esp_err_t safety_supervisor_stop_for_tach_all(
    const char *reason, bool clear_manual_targets);

/** Verified zero-throttle stop suitable for an OTA prepare callback. */
esp_err_t safety_supervisor_prepare_for_ota(bool clear_manual_targets);

/** Release the reversible maintenance inhibit after an OTA abort. */
esp_err_t safety_supervisor_release_maintenance(void);

bool safety_supervisor_interlock_enabled(void);
bool safety_supervisor_interlock_present(void);

void safety_supervisor_set_boot_health(safety_boot_health_t health);
const char *safety_supervisor_boot_health_name(safety_boot_health_t health);

safety_supervisor_health_t safety_supervisor_health_snapshot(void);
int64_t safety_supervisor_heartbeat_us(void);
uint32_t safety_supervisor_stack_bytes(void);

#ifdef __cplusplus
}
#endif
