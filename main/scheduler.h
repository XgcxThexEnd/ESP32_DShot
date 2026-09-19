#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCHEDULER_MAX_INTERVAL_MIN 525600U

typedef enum {
    SCHEDULER_TARGET_MANUAL = 0,
    SCHEDULER_TARGET_START,
    SCHEDULER_TARGET_RESTORE,
} scheduler_target_kind_t;

typedef struct {
    scheduler_target_kind_t kind;
    int local_fan;
    int fan_number;
    int slot;
    uint8_t target_pct;
} scheduler_target_request_t;

typedef struct {
    bool accepted;
    /** Atomic manual-target snapshot used when a schedule starts. */
    uint8_t manual_target_pct;
    /** Atomic command acknowledgement fields used for manual requests. */
    bool communication_inhibit_cleared;
    uint32_t accepted_command_count;
    uint8_t requested_pct;
    uint8_t applied_pct;
} scheduler_target_result_t;

typedef void (*scheduler_apply_target_fn)(
    const scheduler_target_request_t *request,
    scheduler_target_result_t *out_result,
    void *callback_context);

typedef enum {
    SCHEDULER_EVENT_ENTRY_SET = 0,
    SCHEDULER_EVENT_ENTRY_DISABLED,
    SCHEDULER_EVENT_OVERRIDE_CHANGED,
    SCHEDULER_EVENT_STARTED,
    SCHEDULER_EVENT_ENDED,
    SCHEDULER_EVENT_MANUAL_CANCELLED,
    SCHEDULER_EVENT_FAN_CANCELLED,
    SCHEDULER_EVENT_INHIBITED_ALL,
} scheduler_event_type_t;

typedef struct {
    scheduler_event_type_t type;
    int local_fan;
    int fan_number;
    int slot;
    uint8_t target_pct;
    bool inhibited;
    bool target_applied;
} scheduler_event_t;

typedef void (*scheduler_notify_fn)(const scheduler_event_t *event,
                                    void *callback_context);

typedef struct {
    const app_config_t *app_config;
    scheduler_apply_target_fn apply_target;
    scheduler_notify_fn notify;
    void *callback_context;
} scheduler_config_t;

typedef struct {
    bool enabled;
    uint32_t interval_min;
    uint32_t duration_min;
    uint8_t target_pct;
    int64_t next_trigger_us;
    bool active;
    int64_t active_end_us;
} scheduler_entry_snapshot_t;

typedef struct {
    bool feature_enabled;
    bool initialized;
    bool scheduler_task_ready;
    bool save_task_ready;
    bool inhibited;
    int64_t heartbeat_us;
    UBaseType_t scheduler_task_stack_bytes;
    UBaseType_t save_task_stack_bytes;
} scheduler_health_snapshot_t;

/**
 * Initialize scheduler state and load a versioned NVS schedule blob only when
 * its persisted node/motor topology fingerprint matches app_config.
 */
esp_err_t scheduler_init(const scheduler_config_t *config);

/** Start the debounced NVS writer and uptime-relative scheduler tasks. */
esp_err_t scheduler_start(void);

/**
 * Apply a validated manual target and cancel an active schedule only when the
 * target callback accepts it. The callback is never invoked under a scheduler
 * mutex. Callbacks must be bounded and must not synchronously re-enter this
 * module.
 */
esp_err_t scheduler_apply_manual_target(int local_fan, uint8_t target_pct,
                                        scheduler_target_result_t *out_result);

/**
 * Set the persisted global schedule inhibit. Enabling it synchronously restores
 * and cancels every active schedule; uninhibiting restarts all intervals.
 */
esp_err_t scheduler_set_override(bool inhibited);

/** Read the current global schedule inhibit. */
esp_err_t scheduler_get_override(bool *out_inhibited);

/** Define or replace one uptime-relative repeating schedule entry. */
esp_err_t scheduler_set_entry(int local_fan, int slot,
                              uint32_t interval_min, uint32_t duration_min,
                              uint8_t target_pct);

/** Disable one entry, restoring its saved manual target if currently active. */
esp_err_t scheduler_disable_entry(int local_fan, int slot);

/** Copy one immutable entry snapshot. */
esp_err_t scheduler_get_entry(int local_fan, int slot,
                              scheduler_entry_snapshot_t *out_snapshot);

/** Cancel one active schedule without restoring a target or changing its entry. */
esp_err_t scheduler_cancel_fan(int local_fan);

/**
 * Persistently inhibit all schedules, cancel every active runtime slot, and
 * latch the inhibit against clearing for the remainder of this boot.
 */
esp_err_t scheduler_inhibit_all(void);

/**
 * Bounded safety variant of scheduler_inhibit_all(). It never waits longer
 * than timeout_ticks for scheduler coordination plus the NVS commit. Runtime
 * inhibition remains latched even when durable persistence times out.
 */
esp_err_t scheduler_try_inhibit_all(TickType_t timeout_ticks);

/**
 * Persist the latest schedule snapshot through the sole NVS writer task and
 * wait for its commit acknowledgement. timeout_ticks bounds both admission
 * and completion; timing out does not cancel a write already accepted by the
 * writer.
 */
esp_err_t scheduler_save_sync(TickType_t timeout_ticks);

/** Copy task readiness, heartbeat, stack, and inhibit state. */
scheduler_health_snapshot_t scheduler_health_snapshot(void);

/** Lock-independent scheduler-loop heartbeat for safety supervision. */
int64_t scheduler_get_heartbeat_us(void);

#ifdef __cplusplus
}
#endif
