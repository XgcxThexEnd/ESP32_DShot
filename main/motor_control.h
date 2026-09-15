#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MOTOR_CONTROL_MAX_FANS APP_CONFIG_MAX_FANS

typedef struct {
    bool initialized;
    int local_index;
    int fan_index;
    int gpio;
    uint8_t target_pct;
    uint8_t current_pct;
    uint8_t applied_pct;
    uint8_t manual_target_pct;
    uint16_t throttle;
    uint32_t control_generation;
    uint32_t rmt_refresh_count;
    uint32_t rmt_error_total;
    uint32_t accepted_command_count;
    uint16_t rmt_error_consecutive;
    bool rmt_fault_latched;
    bool tach_inhibited;
    int64_t nonzero_applied_since_us;
} motor_control_fan_snapshot_t;

typedef struct {
    bool accepted;
    bool communication_inhibit_cleared;
    uint32_t accepted_command_count;
    uint8_t requested_pct;
    uint8_t applied_pct;
} motor_control_ack_t;

typedef enum {
    MOTOR_CONTROL_EVENT_TARGET_CHANGED = 0,
    MOTOR_CONTROL_EVENT_APPLIED_CHANGED,
    MOTOR_CONTROL_EVENT_RMT_FAULT_LATCHED,
    MOTOR_CONTROL_EVENT_INHIBIT_CHANGED,
    MOTOR_CONTROL_EVENT_RAMP_FATAL,
} motor_control_event_type_t;

typedef struct {
    motor_control_event_type_t type;
    int local_fan; /* -1 for a controller-wide event */
    bool fan_snapshot_valid;
    motor_control_fan_snapshot_t fan;
} motor_control_event_t;

/**
 * Runs in the task that caused the event, including the high-priority ramp
 * task. The callback is always invoked after motor locks are released and must
 * remain bounded/nonblocking (a task notification is the intended pattern).
 */
typedef void (*motor_control_event_callback_t)(
    void *context, const motor_control_event_t *event);

typedef struct {
    bool initialized;
    bool ramp_started;
    bool ramp_paused;
    bool stop_in_progress;
    bool global_inhibited;
    bool maintenance_inhibited;
    bool communication_inhibited;
    bool rmt_inhibited;
    bool rmt_fault_pending;
    int fan_count;
    int initialized_fan_count;
    int64_t ramp_heartbeat_us;
    uint32_t ramp_stack_bytes;
    motor_control_fan_snapshot_t fans[MOTOR_CONTROL_MAX_FANS];
} motor_control_health_snapshot_t;

/**
 * Initialize every configured DShot600 output and start its repeating zero
 * stream. The validated configuration values are copied into the singleton.
 */
esp_err_t motor_control_init(const app_config_t *config);

/** Start the pinned high-priority ramp task after zero-stream arming delay. */
esp_err_t motor_control_start_ramp(motor_control_event_callback_t callback,
                                   void *callback_context);

/**
 * Atomically accept a manual target and acknowledgement snapshot. A valid
 * manual command may clear only the communication inhibit; it cannot clear a
 * global, RMT, or per-fan tach inhibit.
 */
esp_err_t motor_control_request_manual(int local_fan, uint8_t target_pct,
                                       motor_control_ack_t *ack);

/** Set an automatic target (schedule/restore) without clearing any inhibit. */
esp_err_t motor_control_request_automatic(int local_fan, uint8_t target_pct);

/**
 * Start a scheduled target and capture the manual target it displaced in the
 * same motor-state transaction. This closes the race between a fresh manual
 * command and a schedule start.
 */
esp_err_t motor_control_request_scheduled(int local_fan, uint8_t target_pct,
                                          uint8_t *out_manual_target_pct);

/**
 * Restore persisted manual/active targets atomically for every configured fan.
 * The operation is all-or-none and cannot clear any inhibit.
 */
esp_err_t motor_control_restore_all(const uint8_t *targets, size_t target_count);

/** Immediately request and apply zero to one/all outputs. */
esp_err_t motor_control_stop_one(int local_fan, bool clear_manual_target);
esp_err_t motor_control_stop_all(bool clear_manual_targets);

/**
 * OTA-style stop: on an RMT failure, restore only the failed fan's internal
 * current/throttle position when no concurrent generation superseded it.
 * Targets remain zero so subsequent service can retry the stop.
 */
esp_err_t motor_control_stop_all_verified(bool clear_manual_targets);

/** Latch an inhibit and synchronously request/apply zero to every motor. */
esp_err_t motor_control_inhibit_global(bool clear_manual_targets);
/**
 * Latch a reversible maintenance inhibit and synchronously stop every motor.
 * The latch blocks manual and automatic target authorization until explicitly
 * cleared; clearing it never restores a previous target.
 */
esp_err_t motor_control_inhibit_maintenance(bool clear_manual_targets);

/**
 * Clear only the maintenance inhibit after all potentially blocking
 * maintenance preparation has completed. Other safety inhibits are unchanged.
 */
esp_err_t motor_control_clear_maintenance_inhibit(void);

esp_err_t motor_control_inhibit_communication(bool clear_manual_targets);
esp_err_t motor_control_inhibit_rmt(bool clear_manual_targets);

/** Latch every fan's clearable tach inhibit and synchronously stop all. */
esp_err_t motor_control_inhibit_all_tach(bool clear_manual_targets);

/**
 * Set/clear a per-fan tach inhibit. Setting it latches the fan off immediately
 * and clears its manual target. Clearing it never restores a previous target.
 */
esp_err_t motor_control_set_tach_inhibit(int local_fan, bool inhibited);

/** Consume the supervisor notification without clearing any RMT inhibit. */
bool motor_control_take_rmt_fault_pending(void);

/** Pause only ramp/apply iterations; heartbeat and TWDT service continue. */
void motor_control_set_ramp_paused(bool paused);

/** True when the shared hardware interlock may be enabled by its owner. */
bool motor_control_output_allowed(void);

bool motor_control_get_fan_snapshot(int local_fan,
                                    motor_control_fan_snapshot_t *out);
bool motor_control_get_health_snapshot(motor_control_health_snapshot_t *out);

/** Motor/RMT proof only; the caller must validate the external interlock too. */
bool motor_control_holds_safe_zero(void);

int64_t motor_control_ramp_heartbeat_us(void);
uint32_t motor_control_ramp_stack_bytes(void);

#ifdef __cplusplus
}
#endif
