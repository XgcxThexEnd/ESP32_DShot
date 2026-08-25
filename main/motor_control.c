#include "motor_control.h"

#include <limits.h>
#include <string.h>

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"

#include "controller_policy.h"
#include "dshot_esc_encoder.h"

#ifndef CONFIG_RMT_ERROR_LIMIT
#define CONFIG_RMT_ERROR_LIMIT 5
#endif

#define DSHOT_ESC_RESOLUTION_HZ 40000000U
#define DSHOT_ESC_BAUD_RATE     600000U
#define DSHOT_POST_DELAY_US     50U
#define RMT_STOP_TIMEOUT_MS     100U
#define RMT_ERROR_LOG_PERIOD_US (5LL * 1000 * 1000)
#define RMT_REFRESH_PERIOD_US   (1000ULL * 1000ULL)
#define RAMP_TASK_STACK         3072U
#define RAMP_TASK_PRIORITY      18U
#define RAMP_TASK_CORE          0

typedef struct {
    rmt_channel_handle_t channel;
    rmt_encoder_handle_t encoder;
    rmt_encoder_handle_t active_encoder;
    SemaphoreHandle_t io_mutex;
    dshot_esc_throttle_t throttle;
    // rmt_transmit() retains this address for the infinite transaction. The
    // singleton array is never moved and this payload is mutated only after
    // the previous transaction is disabled and drained under io_mutex.
    dshot_esc_throttle_t tx_payload;
    int gpio;
    bool active;
    bool channel_enabled;
    uint8_t current_pct;
    uint8_t target_pct;
    uint8_t applied_pct;
    uint8_t manual_target_pct;
    uint32_t control_generation;
    uint8_t last_reported_pct;
    uint32_t last_dshot;
    uint64_t last_tx_us;
    uint32_t rmt_refresh_count;
    uint32_t rmt_error_total;
    uint32_t accepted_command_count;
    uint16_t rmt_error_consecutive;
    bool rmt_fault_latched;
    bool tach_inhibited;
    int64_t nonzero_applied_since_us;
    int64_t last_rmt_error_log_us;
} motor_context_t;

typedef enum {
    STOP_INHIBIT_NONE = 0,
    STOP_INHIBIT_GLOBAL,
    STOP_INHIBIT_MAINTENANCE,
    STOP_INHIBIT_COMMUNICATION,
    STOP_INHIBIT_RMT,
    STOP_INHIBIT_TACH_ALL,
} stop_inhibit_t;

typedef enum {
    MOTOR_LIFECYCLE_UNINITIALIZED = 0,
    MOTOR_LIFECYCLE_INITIALIZING,
    MOTOR_LIFECYCLE_READY,
    MOTOR_LIFECYCLE_FAILED,
} motor_lifecycle_t;

static const char *TAG = "motor_control";
static motor_context_t s_motors[MOTOR_CONTROL_MAX_FANS];
static portMUX_TYPE s_motor_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_heartbeat_lock = portMUX_INITIALIZER_UNLOCKED;

static motor_lifecycle_t s_lifecycle = MOTOR_LIFECYCLE_UNINITIALIZED;
static int s_fan_count;
static int s_fan_index_start;
static int s_initialized_fan_count;
static int s_owned_fan_count;
static uint8_t s_min_spin_pct;
static uint8_t s_ramp_step_pct;
static uint16_t s_ramp_tick_ms;

static uint32_t s_stop_depth;
static SemaphoreHandle_t s_stop_mutex;
static bool s_global_inhibited;
static uint32_t s_maintenance_depth;
static bool s_communication_inhibited;
static bool s_rmt_inhibited;
static bool s_rmt_fault_pending;
static bool s_ramp_paused;

static TaskHandle_t s_ramp_task;
static bool s_ramp_starting;
static int64_t s_ramp_heartbeat_us;
static motor_control_event_callback_t s_event_callback;
static void *s_event_context;

static bool motor_valid_locked(int local_fan)
{
    return s_lifecycle == MOTOR_LIFECYCLE_READY && local_fan >= 0 &&
           local_fan < s_fan_count;
}

static uint16_t percent_to_dshot(uint8_t percentage)
{
    return controller_pct_to_dshot(percentage, s_min_spin_pct);
}

static void fill_fan_snapshot_locked(int local_fan,
                                     motor_control_fan_snapshot_t *out)
{
    motor_context_t *motor = &s_motors[local_fan];
    *out = (motor_control_fan_snapshot_t) {
        .initialized = local_fan < s_initialized_fan_count,
        .local_index = local_fan,
        .fan_index = s_fan_index_start + local_fan,
        .gpio = motor->gpio,
        .target_pct = motor->target_pct,
        .current_pct = motor->current_pct,
        .applied_pct = motor->applied_pct,
        .manual_target_pct = motor->manual_target_pct,
        .throttle = motor->throttle.throttle,
        .control_generation = motor->control_generation,
        .rmt_refresh_count = motor->rmt_refresh_count,
        .rmt_error_total = motor->rmt_error_total,
        .accepted_command_count = motor->accepted_command_count,
        .rmt_error_consecutive = motor->rmt_error_consecutive,
        .rmt_fault_latched = motor->rmt_fault_latched,
        .tach_inhibited = motor->tach_inhibited,
        .nonzero_applied_since_us = motor->nonzero_applied_since_us,
    };
}

static void emit_event(motor_control_event_type_t type, int local_fan)
{
    motor_control_event_callback_t callback;
    void *context;
    motor_control_event_t event = {
        .type = type,
        .local_fan = local_fan,
    };

    portENTER_CRITICAL(&s_motor_lock);
    callback = s_event_callback;
    context = s_event_context;
    if (local_fan >= 0 && local_fan < s_fan_count) {
        fill_fan_snapshot_locked(local_fan, &event.fan);
        event.fan_snapshot_valid = true;
    }
    portEXIT_CRITICAL(&s_motor_lock);

    if (callback) callback(context, &event);
}

static void log_rmt_error(motor_context_t *motor, const char *operation,
                          esp_err_t error)
{
    int64_t now = esp_timer_get_time();
    if (motor->last_rmt_error_log_us == 0 ||
        now - motor->last_rmt_error_log_us >= RMT_ERROR_LOG_PERIOD_US) {
        motor->last_rmt_error_log_us = now;
        ESP_LOGE(TAG, "GPIO%d RMT %s failed: %s", motor->gpio, operation,
                 esp_err_to_name(error));
    }
}

static bool record_apply_result(motor_context_t *motor, esp_err_t error,
                                uint8_t applied_pct)
{
    bool newly_latched = false;
    portENTER_CRITICAL(&s_motor_lock);
    if (error == ESP_OK) {
        motor->rmt_error_consecutive = 0;
        uint8_t previous_applied = motor->applied_pct;
        motor->applied_pct = applied_pct;
        if (previous_applied == 0 && applied_pct > 0) {
            motor->nonzero_applied_since_us = esp_timer_get_time();
        } else if (applied_pct == 0) {
            motor->nonzero_applied_since_us = 0;
        }
    } else {
        motor->rmt_error_total++;
        if (motor->rmt_error_consecutive < UINT16_MAX) {
            motor->rmt_error_consecutive++;
        }
        if (motor->rmt_error_consecutive >= CONFIG_RMT_ERROR_LIMIT) {
            newly_latched = !motor->rmt_fault_latched;
            motor->rmt_fault_latched = true;
            motor->target_pct = 0;
            motor->control_generation++;
            s_rmt_inhibited = true;
            s_rmt_fault_pending = true;
        }
    }
    portEXIT_CRITICAL(&s_motor_lock);
    return newly_latched;
}

static esp_err_t apply_motor(int local_fan)
{
    motor_context_t *motor = &s_motors[local_fan];
    if (!motor->channel || !motor->encoder || !motor->io_mutex) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(motor->io_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t error = ESP_OK;
    bool coerced_reserved = false;
    dshot_esc_throttle_t requested;
    uint8_t applied_pct;
    portENTER_CRITICAL(&s_motor_lock);
    requested = motor->throttle;
    applied_pct = controller_effective_pct(motor->current_pct, s_min_spin_pct);
    if (requested.throttle > 0 && requested.throttle < 48) {
        requested.throttle = 0;
        requested.telemetry_req = false;
        motor->throttle = requested;
        applied_pct = 0;
        coerced_reserved = true;
    }
    portEXIT_CRITICAL(&s_motor_lock);
    if (coerced_reserved) {
        ESP_LOGE(TAG, "GPIO%d rejected reserved DShot command; forcing stop",
                 motor->gpio);
    }

    uint32_t composite = ((uint32_t)requested.throttle << 1) |
                         (requested.telemetry_req ? 1U : 0U);
    uint64_t now = (uint64_t)esp_timer_get_time();
    bool needs_refresh = now - motor->last_tx_us > RMT_REFRESH_PERIOD_US;
    if (composite == motor->last_dshot && !needs_refresh && motor->active &&
        motor->channel_enabled) {
        goto out;
    }
    bool is_refresh = composite == motor->last_dshot && needs_refresh;
    const rmt_transmit_config_t transmit_config = {
        .loop_count = -1,
    };

    if (motor->channel_enabled) {
        error = rmt_disable(motor->channel);
        if (error != ESP_OK) {
            log_rmt_error(motor, "disable", error);
            goto out;
        }
        motor->channel_enabled = false;
    }
    if (motor->active) {
        error = rmt_tx_wait_all_done(motor->channel, RMT_STOP_TIMEOUT_MS);
        if (error != ESP_OK) {
            log_rmt_error(motor, "stop", error);
            goto out;
        }
        if (motor->active_encoder) {
            error = rmt_encoder_reset(motor->active_encoder);
            if (error != ESP_OK) {
                log_rmt_error(motor, "encoder reset", error);
                goto out;
            }
        }
        motor->active = false;
    }

    motor->tx_payload = requested;
    error = rmt_enable(motor->channel);
    if (error != ESP_OK) {
        log_rmt_error(motor, "enable", error);
        goto out;
    }
    motor->channel_enabled = true;
    rmt_encoder_handle_t encoder =
        motor->active_encoder ? motor->active_encoder : motor->encoder;
    error = rmt_transmit(motor->channel, encoder, &motor->tx_payload,
                         sizeof(motor->tx_payload), &transmit_config);
    if (error != ESP_OK) {
        log_rmt_error(motor, "transmit", error);
        esp_err_t disable_error = rmt_disable(motor->channel);
        if (disable_error == ESP_OK) {
            motor->channel_enabled = false;
            (void)rmt_tx_wait_all_done(motor->channel, RMT_STOP_TIMEOUT_MS);
        }
        goto out;
    }
    motor->active = true;
    motor->last_dshot = composite;
    motor->last_tx_us = now;
    if (is_refresh) {
        portENTER_CRITICAL(&s_motor_lock);
        motor->rmt_refresh_count++;
        portEXIT_CRITICAL(&s_motor_lock);
    }

out:
    {
        bool newly_latched = record_apply_result(motor, error, applied_pct);
        xSemaphoreGive(motor->io_mutex);
        if (newly_latched) {
            emit_event(MOTOR_CONTROL_EVENT_RMT_FAULT_LATCHED, local_fan);
        }
    }
    return error;
}

static void retain_first_error(esp_err_t *first_error, esp_err_t error)
{
    if (first_error && *first_error == ESP_OK && error != ESP_OK) {
        *first_error = error;
    }
}

static bool motor_resources_owned(const motor_context_t *motor)
{
    return motor && (motor->channel || motor->encoder || motor->io_mutex ||
                     motor->channel_enabled || motor->active);
}

/*
 * Initialization is the only lifecycle that tears RMT objects down. Keep a
 * context reachable until every quiesce/delete operation succeeds; losing a
 * handle after a failed disable would make a later safety attempt impossible.
 */
static esp_err_t release_motor_resources(motor_context_t *motor)
{
    if (!motor) return ESP_ERR_INVALID_ARG;

    esp_err_t first_error = ESP_OK;
    SemaphoreHandle_t io_mutex = motor->io_mutex;
    bool mutex_taken = false;
    if (io_mutex) {
        mutex_taken = xSemaphoreTake(io_mutex, portMAX_DELAY) == pdTRUE;
        if (!mutex_taken) return ESP_ERR_TIMEOUT;
    }

    if (motor->channel_enabled && motor->channel) {
        esp_err_t error = rmt_disable(motor->channel);
        retain_first_error(&first_error, error);
        if (error == ESP_OK) motor->channel_enabled = false;
    }

    if (motor->active && !motor->channel_enabled && motor->channel) {
        esp_err_t error = rmt_tx_wait_all_done(motor->channel,
                                               RMT_STOP_TIMEOUT_MS);
        retain_first_error(&first_error, error);
        if (error == ESP_OK && motor->active_encoder) {
            error = rmt_encoder_reset(motor->active_encoder);
            retain_first_error(&first_error, error);
        }
        if (error == ESP_OK) motor->active = false;
    }

    const bool quiesced = !motor->channel_enabled && !motor->active;
    if (quiesced && motor->encoder) {
        esp_err_t error = rmt_del_encoder(motor->encoder);
        retain_first_error(&first_error, error);
        if (error == ESP_OK) {
            motor->encoder = NULL;
            motor->active_encoder = NULL;
        }
    }
    if (quiesced && !motor->encoder && motor->channel) {
        esp_err_t error = rmt_del_channel(motor->channel);
        retain_first_error(&first_error, error);
        if (error == ESP_OK) motor->channel = NULL;
    }

    if (mutex_taken) xSemaphoreGive(io_mutex);
    if (!motor->channel && !motor->encoder && !motor->active &&
        !motor->channel_enabled && io_mutex) {
        vSemaphoreDelete(io_mutex);
        motor->io_mutex = NULL;
    }
    if (!motor_resources_owned(motor)) memset(motor, 0, sizeof(*motor));
    return first_error;
}

static esp_err_t release_owned_motors(void)
{
    esp_err_t first_error = ESP_OK;
    int owned_count;
    portENTER_CRITICAL(&s_motor_lock);
    owned_count = s_owned_fan_count;
    portEXIT_CRITICAL(&s_motor_lock);

    for (int i = owned_count - 1; i >= 0; --i) {
        retain_first_error(&first_error,
                           release_motor_resources(&s_motors[i]));
    }

    int remaining = 0;
    for (int i = 0; i < owned_count; ++i) {
        if (motor_resources_owned(&s_motors[i])) remaining = i + 1;
    }
    portENTER_CRITICAL(&s_motor_lock);
    s_owned_fan_count = remaining;
    s_initialized_fan_count = 0;
    portEXIT_CRITICAL(&s_motor_lock);
    return first_error;
}

static esp_err_t fail_motor_initialization(esp_err_t initialization_error)
{
    esp_err_t cleanup_error = release_owned_motors();
    SemaphoreHandle_t stop_mutex = NULL;

    portENTER_CRITICAL(&s_motor_lock);
    if (cleanup_error == ESP_OK && s_owned_fan_count == 0) {
        stop_mutex = s_stop_mutex;
        s_stop_mutex = NULL;
        s_fan_count = 0;
        s_fan_index_start = 0;
        s_min_spin_pct = 0;
        s_ramp_step_pct = 0;
        s_ramp_tick_ms = 0;
        s_stop_depth = 0;
        s_global_inhibited = false;
        s_maintenance_depth = 0;
        s_communication_inhibited = false;
        s_rmt_inhibited = false;
        s_rmt_fault_pending = false;
        s_ramp_paused = false;
        s_lifecycle = MOTOR_LIFECYCLE_UNINITIALIZED;
    } else {
        /* A retained handle may still control hardware. Make failure terminal
         * and keep every handle reachable for a later fail-safe stop attempt. */
        s_global_inhibited = true;
        s_lifecycle = MOTOR_LIFECYCLE_FAILED;
    }
    portEXIT_CRITICAL(&s_motor_lock);

    if (stop_mutex) vSemaphoreDelete(stop_mutex);
    if (cleanup_error != ESP_OK) {
        ESP_LOGE(TAG, "Motor initialization cleanup failed after %s: %s",
                 esp_err_to_name(initialization_error),
                 esp_err_to_name(cleanup_error));
        return cleanup_error;
    }
    return initialization_error;
}

static esp_err_t initialize_motor(int local_fan, int gpio)
{
    motor_context_t *motor = &s_motors[local_fan];
    motor->gpio = gpio;
    motor->io_mutex = xSemaphoreCreateMutex();
    if (!motor->io_mutex) {
        ESP_LOGE(TAG, "Failed to create RMT mutex for GPIO%d", gpio);
        return ESP_ERR_NO_MEM;
    }

    const rmt_tx_channel_config_t channel_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = gpio,
        .mem_block_symbols = 48,
        .resolution_hz = DSHOT_ESC_RESOLUTION_HZ,
        .trans_queue_depth = 4,
        .flags = { .with_dma = 0 },
    };
    esp_err_t error = rmt_new_tx_channel(&channel_config, &motor->channel);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init RMT TX channel for GPIO%d: %s", gpio,
                 esp_err_to_name(error));
        ESP_LOGE(TAG, "Check that GPIO%d is valid and not already in use", gpio);
        // Centralized transactional unwind owns the mutex from here onward.
        return error;
    }

    const dshot_esc_encoder_config_t encoder_config = {
        .resolution = DSHOT_ESC_RESOLUTION_HZ,
        .baud_rate = DSHOT_ESC_BAUD_RATE,
        .post_delay_us = DSHOT_POST_DELAY_US,
    };
    error = rmt_new_dshot_esc_encoder(&encoder_config, &motor->encoder);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create DShot encoder for GPIO%d: %s", gpio,
                 esp_err_to_name(error));
        // Keep the channel handle reachable until centralized unwind confirms
        // that deletion succeeded.
        return error;
    }
    motor->active_encoder = motor->encoder;
    motor->throttle = (dshot_esc_throttle_t) {
        .throttle = 0,
        .telemetry_req = false,
    };
    motor->current_pct = 0;
    motor->target_pct = 0;
    motor->applied_pct = 0;
    motor->manual_target_pct = 0;
    motor->control_generation = 0;
    motor->last_reported_pct = UINT8_MAX;
    motor->last_dshot = UINT32_MAX;
    motor->last_tx_us = 0;
    motor->rmt_refresh_count = 0;
    motor->rmt_error_total = 0;
    motor->rmt_error_consecutive = 0;
    motor->rmt_fault_latched = false;
    motor->accepted_command_count = 0;
    motor->tach_inhibited = false;
    motor->nonzero_applied_since_us = 0;
    return apply_motor(local_fan);
}

static bool automatic_allowed_locked(int local_fan)
{
    return motor_valid_locked(local_fan) && s_stop_depth == 0 &&
           !s_global_inhibited && s_maintenance_depth == 0 &&
           !s_communication_inhibited && !s_rmt_inhibited &&
           !s_motors[local_fan].rmt_fault_latched &&
           !s_motors[local_fan].tach_inhibited;
}

static esp_err_t stop_all_internal(bool clear_manual_targets,
                                   stop_inhibit_t inhibit,
                                   bool rollback_failed_current)
{
    uint8_t previous_pct[MOTOR_CONTROL_MAX_FANS] = {0};
    uint32_t stop_generation[MOTOR_CONTROL_MAX_FANS] = {0};
    int controlled_count;

    portENTER_CRITICAL(&s_motor_lock);
    if ((s_lifecycle != MOTOR_LIFECYCLE_READY &&
         s_lifecycle != MOTOR_LIFECYCLE_FAILED) ||
        (inhibit == STOP_INHIBIT_MAINTENANCE &&
         s_maintenance_depth == UINT32_MAX) ||
        s_stop_depth == UINT32_MAX) {
        portEXIT_CRITICAL(&s_motor_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_stop_depth++;
    if (inhibit == STOP_INHIBIT_MAINTENANCE) s_maintenance_depth++;
    SemaphoreHandle_t stop_mutex = s_stop_mutex;
    portEXIT_CRITICAL(&s_motor_lock);
    if (stop_mutex) xSemaphoreTake(stop_mutex, portMAX_DELAY);

    portENTER_CRITICAL(&s_motor_lock);
    if (inhibit == STOP_INHIBIT_GLOBAL) s_global_inhibited = true;
    if (inhibit == STOP_INHIBIT_COMMUNICATION) s_communication_inhibited = true;
    if (inhibit == STOP_INHIBIT_RMT) s_rmt_inhibited = true;
    controlled_count = s_lifecycle == MOTOR_LIFECYCLE_FAILED
                           ? s_owned_fan_count
                           : s_initialized_fan_count;
    for (int i = 0; i < controlled_count; ++i) {
        motor_context_t *motor = &s_motors[i];
        previous_pct[i] = motor->current_pct;
        motor->target_pct = 0;
        motor->current_pct = 0;
        motor->throttle.throttle = 0;
        motor->throttle.telemetry_req = false;
        if (clear_manual_targets) motor->manual_target_pct = 0;
        if (inhibit == STOP_INHIBIT_TACH_ALL) motor->tach_inhibited = true;
        stop_generation[i] = ++motor->control_generation;
    }
    portEXIT_CRITICAL(&s_motor_lock);

    esp_err_t first_error = ESP_OK;
    for (int i = 0; i < controlled_count; ++i) {
        esp_err_t error = apply_motor(i);
        if (error != ESP_OK && first_error == ESP_OK) first_error = error;
        if (error != ESP_OK && rollback_failed_current) {
            portENTER_CRITICAL(&s_motor_lock);
            motor_context_t *motor = &s_motors[i];
            if (motor->control_generation == stop_generation[i]) {
                motor->current_pct = previous_pct[i];
                motor->throttle.throttle = percent_to_dshot(previous_pct[i]);
                motor->control_generation++;
            }
            portEXIT_CRITICAL(&s_motor_lock);
        }
    }

    portENTER_CRITICAL(&s_motor_lock);
    if (s_stop_depth > 0) s_stop_depth--;
    portEXIT_CRITICAL(&s_motor_lock);
    if (stop_mutex) xSemaphoreGive(stop_mutex);
    if (inhibit != STOP_INHIBIT_NONE) {
        emit_event(MOTOR_CONTROL_EVENT_INHIBIT_CHANGED, -1);
    }
    for (int i = 0; i < controlled_count; ++i) {
        emit_event(MOTOR_CONTROL_EVENT_APPLIED_CHANGED, i);
    }
    return first_error;
}

static void motor_ramp_task(void *argument)
{
    (void)argument;
#ifdef CONFIG_SUPERVISOR_ENABLED
    esp_err_t watchdog_error = esp_task_wdt_add(NULL);
    if (watchdog_error != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register ramp task with watchdog: %s",
                 esp_err_to_name(watchdog_error));
        esp_err_t inhibit_error = motor_control_inhibit_global(true);
        if (inhibit_error != ESP_OK) {
            ESP_LOGE(TAG, "Ramp fatal zero-output commit failed: %s",
                     esp_err_to_name(inhibit_error));
        }
        emit_event(MOTOR_CONTROL_EVENT_RAMP_FATAL, -1);

        // Do not reboot directly from the ramp task. Its missing heartbeat is
        // consumed by the safety supervisor, which retries a global stop with
        // cleared targets and waits for the synchronous NVS commit before it
        // permits a restart. Remain suspended if that safe restart is denied.
        for (;;) vTaskSuspend(NULL);
    }
#endif

    TickType_t ramp_delay = pdMS_TO_TICKS(s_ramp_tick_ms);
    if (ramp_delay == 0) ramp_delay = 1;
    while (true) {
        portENTER_CRITICAL(&s_motor_lock);
        bool paused = s_ramp_paused;
        portEXIT_CRITICAL(&s_motor_lock);
        if (paused) {
#ifdef CONFIG_SUPERVISOR_ENABLED
            (void)esp_task_wdt_reset();
#endif
            portENTER_CRITICAL(&s_heartbeat_lock);
            s_ramp_heartbeat_us = esp_timer_get_time();
            portEXIT_CRITICAL(&s_heartbeat_lock);
            vTaskDelay(ramp_delay);
            continue;
        }

        for (int i = 0; i < s_fan_count; ++i) {
            int previous_pct;
            uint32_t attempted_generation;
            bool changed;
            portENTER_CRITICAL(&s_motor_lock);
            int current = s_motors[i].current_pct;
            previous_pct = current;
            int target = s_motors[i].target_pct;
            changed = current != target;
            attempted_generation = s_motors[i].control_generation;
            if (changed) {
                int delta = target - current;
                if (delta > 0) {
                    current += delta > s_ramp_step_pct ? s_ramp_step_pct : delta;
                } else {
                    current += delta < -(int)s_ramp_step_pct ?
                        -(int)s_ramp_step_pct : delta;
                }
                if (current < 0) current = 0;
                if (current > 100) current = 100;
                s_motors[i].current_pct = (uint8_t)current;
                s_motors[i].throttle.throttle =
                    percent_to_dshot((uint8_t)current);
                attempted_generation = ++s_motors[i].control_generation;
            }
            portEXIT_CRITICAL(&s_motor_lock);

            if (changed) {
                esp_err_t apply_error = apply_motor(i);
                if (apply_error != ESP_OK) {
                    ESP_LOGE(TAG, "fan%d failed to apply throttle: %s",
                             s_fan_index_start + i, esp_err_to_name(apply_error));
                    portENTER_CRITICAL(&s_motor_lock);
                    if (s_motors[i].control_generation == attempted_generation) {
                        s_motors[i].current_pct = (uint8_t)previous_pct;
                        s_motors[i].throttle.throttle =
                            percent_to_dshot((uint8_t)previous_pct);
                        s_motors[i].control_generation++;
                    }
                    portEXIT_CRITICAL(&s_motor_lock);
                    continue;
                }

                bool publish_change;
                portENTER_CRITICAL(&s_motor_lock);
                uint8_t last = s_motors[i].last_reported_pct;
                uint8_t current = s_motors[i].current_pct;
                bool crossing_on = last == 0 && current > 0;
                bool crossing_off = last > 0 && current == 0;
                bool reached_target = current == s_motors[i].target_pct;
                int difference = (int)current - (int)last;
                if (difference < 0) difference = -difference;
                bool step_change = last == UINT8_MAX || difference >= 5;
                publish_change = crossing_on || crossing_off || step_change ||
                                 reached_target;
                if (publish_change) s_motors[i].last_reported_pct = current;
                portEXIT_CRITICAL(&s_motor_lock);
                if (publish_change) {
                    emit_event(MOTOR_CONTROL_EVENT_APPLIED_CHANGED, i);
                }
            } else {
                esp_err_t apply_error = apply_motor(i);
                if (apply_error != ESP_OK) {
                    ESP_LOGE(TAG, "fan%d failed to refresh DShot: %s",
                             s_fan_index_start + i, esp_err_to_name(apply_error));
                }
            }
        }
#ifdef CONFIG_SUPERVISOR_ENABLED
        (void)esp_task_wdt_reset();
#endif
        // A heartbeat proves that every configured output completed this
        // iteration; recording it before RMT work could mask a stuck channel
        // for one full supervision interval.
        portENTER_CRITICAL(&s_heartbeat_lock);
        s_ramp_heartbeat_us = esp_timer_get_time();
        portEXIT_CRITICAL(&s_heartbeat_lock);
        vTaskDelay(ramp_delay);
    }
}

esp_err_t motor_control_init(const app_config_t *config)
{
    if (!config || config->fan_count < 1 ||
        config->fan_count > MOTOR_CONTROL_MAX_FANS ||
        config->min_spin_pct < 1 || config->min_spin_pct > 100 ||
        config->ramp_step_pct < 1 || config->ramp_step_pct > 20 ||
        config->ramp_tick_ms < 10 || config->ramp_tick_ms > 100) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_motor_lock);
    if (s_lifecycle != MOTOR_LIFECYCLE_UNINITIALIZED) {
        portEXIT_CRITICAL(&s_motor_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_lifecycle = MOTOR_LIFECYCLE_INITIALIZING;
    s_fan_count = config->fan_count;
    s_fan_index_start = config->fan_index_start;
    s_initialized_fan_count = 0;
    s_owned_fan_count = 0;
    s_min_spin_pct = (uint8_t)config->min_spin_pct;
    s_ramp_step_pct = (uint8_t)config->ramp_step_pct;
    s_ramp_tick_ms = (uint16_t)config->ramp_tick_ms;
    s_stop_depth = 0;
    s_global_inhibited = false;
    s_maintenance_depth = 0;
    s_communication_inhibited = false;
    s_rmt_inhibited = false;
    s_rmt_fault_pending = false;
    s_ramp_paused = false;
    s_ramp_task = NULL;
    s_ramp_starting = false;
    s_event_callback = NULL;
    s_event_context = NULL;
    portEXIT_CRITICAL(&s_motor_lock);

    memset(s_motors, 0, sizeof(s_motors));
    SemaphoreHandle_t stop_mutex = xSemaphoreCreateMutex();
    if (!stop_mutex) return fail_motor_initialization(ESP_ERR_NO_MEM);
    portENTER_CRITICAL(&s_motor_lock);
    s_stop_mutex = stop_mutex;
    portEXIT_CRITICAL(&s_motor_lock);

    for (int i = 0; i < config->fan_count; ++i) {
        // Publish ownership before allocation/apply so a failing current fan is
        // always included in transactional unwind and emergency quiesce.
        portENTER_CRITICAL(&s_motor_lock);
        s_owned_fan_count = i + 1;
        portEXIT_CRITICAL(&s_motor_lock);
        esp_err_t error = initialize_motor(i, config->dshot_gpios[i]);
        if (error != ESP_OK) return fail_motor_initialization(error);
        portENTER_CRITICAL(&s_motor_lock);
        s_initialized_fan_count = i + 1;
        portEXIT_CRITICAL(&s_motor_lock);
    }
    portENTER_CRITICAL(&s_motor_lock);
    s_lifecycle = MOTOR_LIFECYCLE_READY;
    portEXIT_CRITICAL(&s_motor_lock);
    return ESP_OK;
}

esp_err_t motor_control_start_ramp(motor_control_event_callback_t callback,
                                   void *callback_context)
{
    if (!callback) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_motor_lock);
    if (s_lifecycle != MOTOR_LIFECYCLE_READY || s_ramp_task ||
        s_ramp_starting) {
        portEXIT_CRITICAL(&s_motor_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_ramp_starting = true;
    s_event_callback = callback;
    s_event_context = callback_context;
    portEXIT_CRITICAL(&s_motor_lock);

    TaskHandle_t task = NULL;
    BaseType_t created = xTaskCreatePinnedToCore(
        motor_ramp_task, "motor_ramp", RAMP_TASK_STACK, NULL,
        RAMP_TASK_PRIORITY, &task, RAMP_TASK_CORE);
    portENTER_CRITICAL(&s_motor_lock);
    s_ramp_starting = false;
    if (created == pdPASS) {
        s_ramp_task = task;
    } else {
        s_event_callback = NULL;
        s_event_context = NULL;
        s_ramp_task = NULL;
    }
    portEXIT_CRITICAL(&s_motor_lock);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t motor_control_request_manual(int local_fan, uint8_t target_pct,
                                       motor_control_ack_t *ack)
{
    if (!ack || target_pct > 100) return ESP_ERR_INVALID_ARG;
    *ack = (motor_control_ack_t) {0};

    portENTER_CRITICAL(&s_motor_lock);
    if (!motor_valid_locked(local_fan)) {
        portEXIT_CRITICAL(&s_motor_lock);
        return ESP_ERR_INVALID_STATE;
    }
    motor_context_t *motor = &s_motors[local_fan];
    bool allowed = s_stop_depth == 0 && !s_global_inhibited &&
                   s_maintenance_depth == 0 && !s_rmt_inhibited &&
                   !motor->rmt_fault_latched && !motor->tach_inhibited;
    if (!allowed) {
        portEXIT_CRITICAL(&s_motor_lock);
        return ESP_ERR_INVALID_STATE;
    }

    bool communication_cleared = s_communication_inhibited;
    s_communication_inhibited = false;
    motor->manual_target_pct = target_pct;
    motor->target_pct = target_pct;
    if (motor->accepted_command_count < UINT32_MAX) {
        motor->accepted_command_count++;
    }
    *ack = (motor_control_ack_t) {
        .accepted = true,
        .communication_inhibit_cleared = communication_cleared,
        .accepted_command_count = motor->accepted_command_count,
        .requested_pct = motor->target_pct,
        .applied_pct = motor->applied_pct,
    };
    portEXIT_CRITICAL(&s_motor_lock);
    emit_event(MOTOR_CONTROL_EVENT_TARGET_CHANGED, local_fan);
    return ESP_OK;
}

esp_err_t motor_control_request_automatic(int local_fan, uint8_t target_pct)
{
    if (target_pct > 100) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_motor_lock);
    if (!automatic_allowed_locked(local_fan)) {
        portEXIT_CRITICAL(&s_motor_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_motors[local_fan].target_pct = target_pct;
    portEXIT_CRITICAL(&s_motor_lock);
    emit_event(MOTOR_CONTROL_EVENT_TARGET_CHANGED, local_fan);
    return ESP_OK;
}

esp_err_t motor_control_request_scheduled(int local_fan, uint8_t target_pct,
                                          uint8_t *out_manual_target_pct)
{
    if (!out_manual_target_pct || target_pct > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_motor_lock);
    if (!automatic_allowed_locked(local_fan)) {
        portEXIT_CRITICAL(&s_motor_lock);
        return ESP_ERR_INVALID_STATE;
    }
    *out_manual_target_pct = s_motors[local_fan].manual_target_pct;
    s_motors[local_fan].target_pct = target_pct;
    portEXIT_CRITICAL(&s_motor_lock);
    emit_event(MOTOR_CONTROL_EVENT_TARGET_CHANGED, local_fan);
    return ESP_OK;
}

esp_err_t motor_control_restore_all(const uint8_t *targets, size_t target_count)
{
    if (!targets) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_motor_lock);
    if (s_lifecycle != MOTOR_LIFECYCLE_READY ||
        target_count != (size_t)s_fan_count) {
        portEXIT_CRITICAL(&s_motor_lock);
        return ESP_ERR_INVALID_ARG;
    }
    bool allowed = s_stop_depth == 0 && !s_global_inhibited &&
                   s_maintenance_depth == 0 && !s_communication_inhibited &&
                   !s_rmt_inhibited;
    for (int i = 0; allowed && i < s_fan_count; ++i) {
        allowed = targets[i] <= 100 && !s_motors[i].rmt_fault_latched &&
                  !s_motors[i].tach_inhibited;
    }
    if (!allowed) {
        portEXIT_CRITICAL(&s_motor_lock);
        return ESP_ERR_INVALID_STATE;
    }
    for (int i = 0; i < s_fan_count; ++i) {
        s_motors[i].manual_target_pct = targets[i];
        s_motors[i].target_pct = targets[i];
    }
    portEXIT_CRITICAL(&s_motor_lock);
    for (int i = 0; i < s_fan_count; ++i) {
        emit_event(MOTOR_CONTROL_EVENT_TARGET_CHANGED, i);
    }
    return ESP_OK;
}

esp_err_t motor_control_stop_one(int local_fan, bool clear_manual_target)
{
    portENTER_CRITICAL(&s_motor_lock);
    if (!motor_valid_locked(local_fan)) {
        portEXIT_CRITICAL(&s_motor_lock);
        return ESP_ERR_INVALID_STATE;
    }
    motor_context_t *motor = &s_motors[local_fan];
    motor->target_pct = 0;
    motor->current_pct = 0;
    motor->throttle.throttle = 0;
    motor->throttle.telemetry_req = false;
    if (clear_manual_target) motor->manual_target_pct = 0;
    motor->control_generation++;
    portEXIT_CRITICAL(&s_motor_lock);
    esp_err_t error = apply_motor(local_fan);
    emit_event(MOTOR_CONTROL_EVENT_APPLIED_CHANGED, local_fan);
    return error;
}

esp_err_t motor_control_stop_all(bool clear_manual_targets)
{
    return stop_all_internal(clear_manual_targets, STOP_INHIBIT_NONE, false);
}

esp_err_t motor_control_stop_all_verified(bool clear_manual_targets)
{
    return stop_all_internal(clear_manual_targets, STOP_INHIBIT_NONE, true);
}

esp_err_t motor_control_inhibit_global(bool clear_manual_targets)
{
    return stop_all_internal(clear_manual_targets, STOP_INHIBIT_GLOBAL, false);
}

esp_err_t motor_control_inhibit_maintenance(bool clear_manual_targets)
{
    return stop_all_internal(clear_manual_targets, STOP_INHIBIT_MAINTENANCE,
                             false);
}

esp_err_t motor_control_clear_maintenance_inhibit(void)
{
    bool changed = false;
    portENTER_CRITICAL(&s_motor_lock);
    if (s_lifecycle != MOTOR_LIFECYCLE_READY) {
        portEXIT_CRITICAL(&s_motor_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_maintenance_depth > 0) {
        s_maintenance_depth--;
        changed = s_maintenance_depth == 0;
    }
    portEXIT_CRITICAL(&s_motor_lock);
    if (changed) emit_event(MOTOR_CONTROL_EVENT_INHIBIT_CHANGED, -1);
    return ESP_OK;
}

esp_err_t motor_control_inhibit_communication(bool clear_manual_targets)
{
    return stop_all_internal(clear_manual_targets,
                             STOP_INHIBIT_COMMUNICATION, false);
}

esp_err_t motor_control_inhibit_rmt(bool clear_manual_targets)
{
    return stop_all_internal(clear_manual_targets, STOP_INHIBIT_RMT, false);
}

esp_err_t motor_control_inhibit_all_tach(bool clear_manual_targets)
{
    return stop_all_internal(clear_manual_targets, STOP_INHIBIT_TACH_ALL,
                             false);
}

esp_err_t motor_control_set_tach_inhibit(int local_fan, bool inhibited)
{
    portENTER_CRITICAL(&s_motor_lock);
    if (!motor_valid_locked(local_fan)) {
        portEXIT_CRITICAL(&s_motor_lock);
        return ESP_ERR_INVALID_STATE;
    }
    motor_context_t *motor = &s_motors[local_fan];
    motor->tach_inhibited = inhibited;
    if (inhibited) {
        motor->target_pct = 0;
        motor->manual_target_pct = 0;
        motor->current_pct = 0;
        motor->throttle.throttle = 0;
        motor->throttle.telemetry_req = false;
        motor->control_generation++;
    }
    portEXIT_CRITICAL(&s_motor_lock);

    esp_err_t error = ESP_OK;
    if (inhibited) error = apply_motor(local_fan);
    emit_event(MOTOR_CONTROL_EVENT_INHIBIT_CHANGED, local_fan);
    if (inhibited) emit_event(MOTOR_CONTROL_EVENT_APPLIED_CHANGED, local_fan);
    return error;
}

bool motor_control_take_rmt_fault_pending(void)
{
    portENTER_CRITICAL(&s_motor_lock);
    bool pending = s_rmt_fault_pending;
    s_rmt_fault_pending = false;
    portEXIT_CRITICAL(&s_motor_lock);
    return pending;
}

void motor_control_set_ramp_paused(bool paused)
{
    portENTER_CRITICAL(&s_motor_lock);
    s_ramp_paused = paused;
    portEXIT_CRITICAL(&s_motor_lock);
}

bool motor_control_output_allowed(void)
{
    portENTER_CRITICAL(&s_motor_lock);
    bool allowed = s_lifecycle == MOTOR_LIFECYCLE_READY &&
                   s_stop_depth == 0 && !s_global_inhibited &&
                   s_maintenance_depth == 0 &&
                   !s_communication_inhibited && !s_rmt_inhibited;
    for (int i = 0; allowed && i < s_initialized_fan_count; ++i) {
        allowed = !s_motors[i].rmt_fault_latched;
    }
    portEXIT_CRITICAL(&s_motor_lock);
    return allowed;
}

bool motor_control_get_fan_snapshot(int local_fan,
                                    motor_control_fan_snapshot_t *out)
{
    if (!out) return false;
    portENTER_CRITICAL(&s_motor_lock);
    if (!motor_valid_locked(local_fan)) {
        portEXIT_CRITICAL(&s_motor_lock);
        return false;
    }
    fill_fan_snapshot_locked(local_fan, out);
    portEXIT_CRITICAL(&s_motor_lock);
    return true;
}

bool motor_control_get_health_snapshot(motor_control_health_snapshot_t *out)
{
    if (!out) return false;
    motor_control_health_snapshot_t snapshot = {0};
    TaskHandle_t ramp_task;
    portENTER_CRITICAL(&s_motor_lock);
    bool ready = s_lifecycle == MOTOR_LIFECYCLE_READY;
    snapshot.initialized = ready;
    snapshot.ramp_started = ready && s_ramp_task != NULL;
    snapshot.ramp_paused = s_ramp_paused;
    snapshot.stop_in_progress = s_stop_depth > 0;
    snapshot.global_inhibited = s_global_inhibited;
    snapshot.maintenance_inhibited = s_maintenance_depth > 0;
    snapshot.communication_inhibited = s_communication_inhibited;
    snapshot.rmt_inhibited = s_rmt_inhibited;
    snapshot.rmt_fault_pending = s_rmt_fault_pending;
    snapshot.fan_count = ready ? s_fan_count : 0;
    snapshot.initialized_fan_count = ready ? s_initialized_fan_count : 0;
    if (ready) {
        for (int i = 0; i < s_fan_count; ++i) {
            fill_fan_snapshot_locked(i, &snapshot.fans[i]);
        }
    }
    ramp_task = ready ? s_ramp_task : NULL;
    portEXIT_CRITICAL(&s_motor_lock);
    portENTER_CRITICAL(&s_heartbeat_lock);
    snapshot.ramp_heartbeat_us = s_ramp_heartbeat_us;
    portEXIT_CRITICAL(&s_heartbeat_lock);
    if (ramp_task) {
        snapshot.ramp_stack_words =
            (uint32_t)uxTaskGetStackHighWaterMark(ramp_task);
    }
    *out = snapshot;
    return true;
}

bool motor_control_holds_safe_zero(void)
{
    SemaphoreHandle_t io_mutexes[MOTOR_CONTROL_MAX_FANS] = {0};
    portENTER_CRITICAL(&s_motor_lock);
    bool initialized = s_lifecycle == MOTOR_LIFECYCLE_READY &&
                       s_initialized_fan_count == s_fan_count;
    int fan_count = s_fan_count;
    for (int i = 0; initialized && i < fan_count; ++i) {
        io_mutexes[i] = s_motors[i].io_mutex;
        if (!io_mutexes[i]) initialized = false;
    }
    portEXIT_CRITICAL(&s_motor_lock);
    if (!initialized) return false;

    int locked_count = 0;
    for (; locked_count < fan_count; ++locked_count) {
        if (xSemaphoreTake(io_mutexes[locked_count],
                           pdMS_TO_TICKS(RMT_STOP_TIMEOUT_MS + 50U)) != pdTRUE) {
            for (int i = locked_count - 1; i >= 0; --i) {
                xSemaphoreGive(io_mutexes[i]);
            }
            return false;
        }
    }

    // apply_motor() owns io_mutex before s_motor_lock. Owning every I/O mutex
    // in the same fan order gives one coherent all-output proof rather than a
    // sequence of individually valid observations.
    bool healthy;
    portENTER_CRITICAL(&s_motor_lock);
    healthy = s_lifecycle == MOTOR_LIFECYCLE_READY &&
              s_initialized_fan_count == fan_count &&
              s_fan_count == fan_count;
    for (int i = 0; healthy && i < fan_count; ++i) {
        motor_context_t *motor = &s_motors[i];
        healthy = motor->io_mutex == io_mutexes[i] && motor->channel &&
                  motor->encoder && motor->active &&
                  motor->channel_enabled && motor->target_pct == 0 &&
                  motor->current_pct == 0 && motor->applied_pct == 0 &&
                  motor->throttle.throttle == 0 &&
                  motor->tx_payload.throttle == 0 && motor->last_dshot == 0 &&
                  motor->rmt_error_consecutive == 0 &&
                  !motor->rmt_fault_latched;
    }
    portEXIT_CRITICAL(&s_motor_lock);
    for (int i = fan_count - 1; i >= 0; --i) xSemaphoreGive(io_mutexes[i]);
    return healthy;
}

int64_t motor_control_ramp_heartbeat_us(void)
{
    portENTER_CRITICAL(&s_heartbeat_lock);
    int64_t heartbeat = s_ramp_heartbeat_us;
    portEXIT_CRITICAL(&s_heartbeat_lock);
    return heartbeat;
}

uint32_t motor_control_ramp_stack_words(void)
{
    portENTER_CRITICAL(&s_motor_lock);
    TaskHandle_t task = s_ramp_task;
    portEXIT_CRITICAL(&s_motor_lock);
    return task ? (uint32_t)uxTaskGetStackHighWaterMark(task) : 0;
}
