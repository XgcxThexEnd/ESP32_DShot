#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "scheduler.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STATE_PUBLISHER_BOOT_HEALTH_CAPACITY 32
#define STATE_PUBLISHER_STOP_REASON_CAPACITY 80

/**
 * Controller-owned health fields which cannot be read from an extracted
 * subsystem. The coordinator fills this snapshot without exposing mutable
 * state to the publisher.
 */
typedef struct {
    char boot_health[STATE_PUBLISHER_BOOT_HEALTH_CAPACITY];
    bool interlock_configured;
    bool interlock_enabled;
    bool stop_in_progress;
    bool communication_failsafe_active;
    bool global_safety_latched;
    bool ramp_liveness_fault;
    bool scheduler_liveness_fault;
    bool tach_liveness_fault;
    bool tach_action_ready;
    bool tach_action_pending;
    int64_t tach_action_age_ms;
    uint32_t tach_action_stack_words;
    bool mqtt_command_overflow_fault;
    bool mqtt_dispatch_liveness_fault;
    UBaseType_t supervisor_stack_words;
    uint32_t stop_count;
    esp_err_t last_stop_result;
    char last_stop_reason[STATE_PUBLISHER_STOP_REASON_CAPACITY];
} state_publisher_coordinator_health_t;

/**
 * Fill one bounded, point-in-time coordinator health snapshot. The callback
 * runs in a normal publisher/coordinator task context and is never invoked
 * while a state-publisher lock is held.
 */
typedef void (*state_publisher_read_coordinator_health_fn)(
    void *callback_context,
    state_publisher_coordinator_health_t *out_snapshot);

typedef struct {
    const app_config_t *app_config;
    state_publisher_read_coordinator_health_fn read_coordinator_health;
    void *callback_context;
} state_publisher_config_t;

/** Initialize the singleton after app_config and subsystem initialization. */
esp_err_t state_publisher_init(const state_publisher_config_t *config);

/** Start the task which coalesces per-fan state publications. */
esp_err_t state_publisher_start(void);

/**
 * Request a per-fan snapshot publication. This is a bounded task notification
 * and is safe to call from the high-priority motor ramp callback (not an ISR).
 */
void state_publisher_request_fan(int local_fan, bool full_retained);

/** Request retained QoS-1 state snapshots for every configured fan. */
void state_publisher_request_all(void);

/** Request HA discovery, availability, node metadata, fan list, and node-up. */
void state_publisher_request_discovery_and_metadata(void);

/** Request retained motor metrics and the non-retained QoS-1 health probe. */
void state_publisher_request_periodic_metrics_health(void);

/** Publish the typed acknowledgement for one accepted manual command. */
bool state_publisher_publish_manual_ack(int local_fan,
                                        uint32_t accepted_command_count,
                                        uint8_t requested_pct,
                                        uint8_t applied_pct);

/** Publish the retained response payload for one schedule entry. */
bool state_publisher_publish_schedule_entry(
    int local_fan, unsigned int slot,
    const scheduler_entry_snapshot_t *entry);

/** Publish a node-scoped OTA status with QoS 1. */
bool state_publisher_publish_ota_status(const char *status, bool retain);

/** State-task readiness used by the local boot-health window. */
bool state_publisher_is_ready(void);

/** State-task stack high-water mark in FreeRTOS words, or zero if stopped. */
UBaseType_t state_publisher_stack_high_water_mark(void);

#ifdef __cplusplus
}
#endif
