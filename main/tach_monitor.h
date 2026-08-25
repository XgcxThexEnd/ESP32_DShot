#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t applied_pct;
    int64_t nonzero_applied_since_us;
} tach_monitor_motor_snapshot_t;

typedef enum {
    TACH_MONITOR_STALL_ALARM_ONLY = 0,
    TACH_MONITOR_STALL_STOP_FAN,
    TACH_MONITOR_STALL_STOP_ALL,
} tach_monitor_stall_action_t;

typedef enum {
    TACH_MONITOR_FAULT_NONE = 0,
    TACH_MONITOR_FAULT_STALL,
    TACH_MONITOR_FAULT_SENSOR_INVALID,
} tach_monitor_fault_cause_t;

typedef struct {
    int local_fan;
    int fan_number;
    uint32_t measured_rpm;
    uint32_t stall_count;
    /** Changes on every fault latch and explicit clear. */
    uint32_t fault_generation;
    tach_monitor_stall_action_t action;
    tach_monitor_fault_cause_t cause;
} tach_monitor_stall_event_t;

typedef bool (*tach_monitor_read_motor_snapshot_fn)(
    int local_fan, tach_monitor_motor_snapshot_t *out_snapshot,
    void *callback_context);

typedef void (*tach_monitor_confirmed_stall_fn)(
    const tach_monitor_stall_event_t *event, void *callback_context);

typedef struct {
    const app_config_t *app_config;
    tach_monitor_read_motor_snapshot_fn read_motor_snapshot;
    tach_monitor_confirmed_stall_fn confirmed_stall;
    void *callback_context;
} tach_monitor_config_t;

typedef struct {
    bool measurement_valid;
    uint32_t measured_rpm;
    bool stall_alarm;
    bool stall_latched;
    uint32_t stall_count;
    uint32_t fault_generation;
    int64_t stall_since_us;
    int64_t invalid_since_us;
    int64_t sampled_at_us;
    tach_monitor_fault_cause_t fault_cause;
} tach_monitor_snapshot_t;

/** Configure GPIO/PCNT resources. The referenced app_config is immutable. */
esp_err_t tach_monitor_init(const tach_monitor_config_t *config);

/** Start the sampling task after successful initialization. */
esp_err_t tach_monitor_start(void);

/**
 * Clear only the tach monitor's alarm/latch/debounce state for one local fan.
 * A coordinator must clear any corresponding motor-control inhibit separately.
 */
esp_err_t tach_monitor_clear_alarm(int local_fan);

/** Copy a coherent per-fan tach state snapshot. */
esp_err_t tach_monitor_get_snapshot(int local_fan,
                                    tach_monitor_snapshot_t *out_snapshot);

/** Last successful sampling-loop heartbeat, in esp_timer microseconds. */
int64_t tach_monitor_get_heartbeat_us(void);

/** Sampling-task stack high-water mark in FreeRTOS words, or zero if stopped. */
UBaseType_t tach_monitor_get_stack_high_water_mark(void);

#ifdef __cplusplus
}
#endif
