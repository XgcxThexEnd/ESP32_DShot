#include "safety_supervisor.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#ifdef CONFIG_MOTOR_INTERLOCK_ENABLED
#include "driver/gpio.h"
#endif
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "motor_control.h"
#include "mqtt_manager.h"
#include "scheduler.h"
#include "tach_monitor.h"

#define SUPERVISOR_TASK_STACK    4096U
#define SUPERVISOR_TASK_PRIORITY 19
#define SUPERVISOR_PERIOD_MS     250U
#define SAFETY_AUTOMATIC_INHIBIT_TIMEOUT_MS 1000U
#define SAFETY_PERSIST_TIMEOUT_MS            3000U
#define SUPERVISOR_RESTART_RETRY_MS          5000U
#define SAFETY_TACH_ACTION_TIMEOUT_MS        10000U
#define SAFETY_MQTT_DISPATCH_TIMEOUT_MS      15000U

typedef enum {
    STOP_ACTION_PLAIN = 0,
    STOP_ACTION_GLOBAL,
    STOP_ACTION_COMMUNICATION,
    STOP_ACTION_RMT,
    STOP_ACTION_TACH_ALL,
    STOP_ACTION_VERIFIED,
} stop_action_t;

typedef struct {
    esp_err_t interlock_error;
    esp_err_t maintenance_error;
    esp_err_t motor_error;
    esp_err_t automatic_error;
    esp_err_t fallback_error;
    esp_err_t persistence_error;
    esp_err_t release_error;
    esp_err_t result;
    bool automatic_committed;
    bool persistence_committed;
} stop_outcome_t;

static const char *TAG = "safety_supervisor";
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_stop_mutex;
static safety_supervisor_callbacks_t s_callbacks;
static TaskHandle_t s_supervisor_task;
static safety_boot_health_t s_boot_health = SAFETY_BOOT_INITIALIZING;
static bool s_initialized;
static bool s_starting;
static bool s_started;
static bool s_watchdog_configured;
static bool s_interlock_enabled;
static uint32_t s_stop_depth;
static uint32_t s_safety_generation;
static uint32_t s_stop_count;
static int64_t s_heartbeat_us;
static int64_t s_supervision_started_us;
static int64_t s_last_restart_attempt_us;
static bool s_ramp_liveness_fault;
static bool s_scheduler_liveness_fault;
static bool s_tach_liveness_fault;
static bool s_mqtt_command_overflow_fault;
static bool s_mqtt_dispatch_liveness_fault;
static uint32_t s_observed_mqtt_command_queue_drops;
#ifdef CONFIG_RMT_FAILURE_REBOOT
static bool s_rmt_reboot_pending;
#endif
static esp_err_t s_last_stop_result = ESP_OK;
static char s_last_stop_reason[SAFETY_SUPERVISOR_REASON_CAPACITY] = "none";

static esp_err_t interlock_drive(bool enabled)
{
#ifdef CONFIG_MOTOR_INTERLOCK_ENABLED
#ifdef CONFIG_MOTOR_INTERLOCK_ACTIVE_HIGH
    const int active_level = 1;
#else
    const int active_level = 0;
#endif
    esp_err_t error = gpio_set_level(
        CONFIG_MOTOR_INTERLOCK_GPIO,
        enabled ? active_level : !active_level);
    if (error == ESP_OK) {
        portENTER_CRITICAL(&s_state_lock);
        s_interlock_enabled = enabled;
        portEXIT_CRITICAL(&s_state_lock);
    }
    return error;
#else
    (void)enabled;
    // Without an independent output there is no hardware line to deassert.
    // Preserve the existing logical state convention and expose presence as a
    // separate health field so consumers can distinguish that configuration.
    portENTER_CRITICAL(&s_state_lock);
    s_interlock_enabled = true;
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
#endif
}

static esp_err_t stop_motors(stop_action_t action, bool clear_manual_targets)
{
    switch (action) {
        case STOP_ACTION_GLOBAL:
            return motor_control_inhibit_global(clear_manual_targets);
        case STOP_ACTION_COMMUNICATION:
            return motor_control_inhibit_communication(clear_manual_targets);
        case STOP_ACTION_RMT:
            return motor_control_inhibit_rmt(clear_manual_targets);
        case STOP_ACTION_TACH_ALL:
            return motor_control_inhibit_all_tach(clear_manual_targets);
        case STOP_ACTION_VERIFIED:
            return motor_control_stop_all_verified(clear_manual_targets);
        case STOP_ACTION_PLAIN:
        default:
            return motor_control_stop_all(clear_manual_targets);
    }
}

static esp_err_t preserve_first_error(esp_err_t current, esp_err_t candidate)
{
    return current != ESP_OK ? current : candidate;
}

static void feed_supervisor_watchdog_if_current(void)
{
#ifdef CONFIG_SUPERVISOR_ENABLED
    portENTER_CRITICAL(&s_state_lock);
    TaskHandle_t supervisor_task = s_supervisor_task;
    portEXIT_CRITICAL(&s_state_lock);
    if (supervisor_task &&
        xTaskGetCurrentTaskHandle() == supervisor_task) {
        (void)esp_task_wdt_reset();
    }
#endif
}

static esp_err_t stop_all_internal(const char *reason,
                                   bool clear_manual_targets,
                                   stop_action_t action,
                                   stop_outcome_t *out_outcome)
{
    const char *safe_reason = reason ? reason : "unspecified";
    char reason_copy[SAFETY_SUPERVISOR_REASON_CAPACITY] = {0};
    safety_supervisor_callbacks_t callbacks;

    (void)snprintf(reason_copy, sizeof(reason_copy), "%s", safe_reason);
    ESP_LOGE(TAG, "Safety stop: %s", safe_reason);

    // Claim the stop before touching hardware. An enable racing this path will
    // observe either stop_depth or the changed generation and be rejected.
    portENTER_CRITICAL(&s_state_lock);
    if (s_stop_depth < UINT32_MAX) s_stop_depth++;
    s_safety_generation++;
    if (s_stop_count < UINT32_MAX) s_stop_count++;
    callbacks = s_callbacks;
    SemaphoreHandle_t stop_mutex = s_stop_mutex;
    portEXIT_CRITICAL(&s_state_lock);

    // Serialize the physical GPIO write with all enable attempts. The stop
    // claim above makes enable attempts fail while callbacks and RMT work run.
    if (stop_mutex) {
        xSemaphoreTakeRecursive(stop_mutex, portMAX_DELAY);
    }
    esp_err_t interlock_error = interlock_drive(false);
    if (stop_mutex) xSemaphoreGiveRecursive(stop_mutex);
    feed_supervisor_watchdog_if_current();

    const bool durable_inhibit = action == STOP_ACTION_GLOBAL ||
                                 action == STOP_ACTION_COMMUNICATION ||
                                 action == STOP_ACTION_RMT ||
                                 action == STOP_ACTION_TACH_ALL;
    const bool temporary_maintenance = !durable_inhibit;
    esp_err_t maintenance_error = ESP_OK;
    esp_err_t motor_error = ESP_OK;
    esp_err_t automatic_error = ESP_OK;
    esp_err_t fallback_error = ESP_OK;
    esp_err_t persistence_error = ESP_OK;
    esp_err_t release_error = ESP_OK;

    if (durable_inhibit) {
        // Publish the durable inhibit under motor_control's authorization lock
        // before yielding to any application callback.
        if (stop_mutex) {
            xSemaphoreTakeRecursive(stop_mutex, portMAX_DELAY);
        }
        motor_error = stop_motors(action, clear_manual_targets);
        if (stop_mutex) xSemaphoreGiveRecursive(stop_mutex);
    } else {
        // Claim a reversible motor-side inhibit first. A scheduler task which
        // is dead while holding its mutex can no longer delay physical safety.
        if (stop_mutex) {
            xSemaphoreTakeRecursive(stop_mutex, portMAX_DELAY);
        }
        maintenance_error =
            motor_control_inhibit_maintenance(clear_manual_targets);
        if (stop_mutex) xSemaphoreGiveRecursive(stop_mutex);
    }
    feed_supervisor_watchdog_if_current();

    if (callbacks.inhibit_automatic) {
        automatic_error = callbacks.inhibit_automatic(
            callbacks.callback_context,
            pdMS_TO_TICKS(SAFETY_AUTOMATIC_INHIBIT_TIMEOUT_MS));
    }
    feed_supervisor_watchdog_if_current();

    // The first maintenance stop is fail-closed. OTA additionally asks RMT to
    // prove the final zero stream using the verified rollback semantics.
    if (temporary_maintenance && maintenance_error == ESP_OK &&
        automatic_error == ESP_OK && action == STOP_ACTION_VERIFIED) {
        if (stop_mutex) {
            xSemaphoreTakeRecursive(stop_mutex, portMAX_DELAY);
        }
        motor_error = stop_motors(action, clear_manual_targets);
        if (stop_mutex) xSemaphoreGiveRecursive(stop_mutex);
    }

    esp_err_t result = ESP_OK;
    result = preserve_first_error(result, interlock_error);
    result = preserve_first_error(result, maintenance_error);
    result = preserve_first_error(result, motor_error);
    if (durable_inhibit && automatic_error != ESP_OK) {
        // The motor-side latch already makes this boot fail-safe, but callers
        // which intend to reboot must see that the schedule inhibit was not
        // durably committed. Suppressing that error could let an older NVS
        // schedule become active after the reboot.
        ESP_LOGE(TAG, "Automatic-source inhibit failed after durable stop: %s",
                 esp_err_to_name(automatic_error));
    }
    result = preserve_first_error(result, automatic_error);

    // Any failed temporary-stop step escalates to a durable global latch.
    if (temporary_maintenance && result != ESP_OK) {
        if (stop_mutex) xSemaphoreTakeRecursive(stop_mutex, portMAX_DELAY);
        fallback_error = stop_motors(STOP_ACTION_GLOBAL,
                                     clear_manual_targets);
        if (stop_mutex) xSemaphoreGiveRecursive(stop_mutex);
        result = preserve_first_error(result, fallback_error);
    }
    feed_supervisor_watchdog_if_current();

    // Wait for an acknowledged NVS commit before reporting a cleared-target
    // stop as complete. Debounced persistence cannot protect a near-term reset.
    if (clear_manual_targets && callbacks.request_persistence) {
        feed_supervisor_watchdog_if_current();
        persistence_error = callbacks.request_persistence(
            callbacks.callback_context,
            pdMS_TO_TICKS(SAFETY_PERSIST_TIMEOUT_MS));
        if (temporary_maintenance && persistence_error != ESP_OK &&
            fallback_error == ESP_OK) {
            if (stop_mutex) xSemaphoreTakeRecursive(stop_mutex, portMAX_DELAY);
            fallback_error = stop_motors(STOP_ACTION_GLOBAL, true);
            if (stop_mutex) xSemaphoreGiveRecursive(stop_mutex);
        }
        result = preserve_first_error(result, fallback_error);
        result = preserve_first_error(result, persistence_error);
    }
    feed_supervisor_watchdog_if_current();

    // Plain stops are transient after every safety step succeeds. OTA retains
    // maintenance inhibition until its abort callback or the verified reboot.
    if (action == STOP_ACTION_PLAIN && result == ESP_OK) {
        release_error = motor_control_clear_maintenance_inhibit();
        result = preserve_first_error(result, release_error);
    }

    portENTER_CRITICAL(&s_state_lock);
    s_last_stop_result = result;
    memcpy(s_last_stop_reason, reason_copy, sizeof(s_last_stop_reason));
    if (s_stop_depth > 0) s_stop_depth--;
    portEXIT_CRITICAL(&s_state_lock);

    if (callbacks.notify_state) {
        callbacks.notify_state(callbacks.callback_context);
    }
    if (out_outcome) {
        *out_outcome = (stop_outcome_t) {
            .interlock_error = interlock_error,
            .maintenance_error = maintenance_error,
            .motor_error = motor_error,
            .automatic_error = automatic_error,
            .fallback_error = fallback_error,
            .persistence_error = persistence_error,
            .release_error = release_error,
            .result = result,
            .automatic_committed = callbacks.inhibit_automatic &&
                                   automatic_error == ESP_OK,
            .persistence_committed =
                !clear_manual_targets ||
                (callbacks.request_persistence &&
                 persistence_error == ESP_OK),
        };
    }
    return result;
}

esp_err_t safety_supervisor_init(void)
{
    portENTER_CRITICAL(&s_state_lock);
    bool already_initialized = s_initialized;
    portEXIT_CRITICAL(&s_state_lock);
    if (already_initialized) return ESP_OK;

#ifdef CONFIG_MOTOR_INTERLOCK_ENABLED
    if (!GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_MOTOR_INTERLOCK_GPIO)) {
        return ESP_ERR_INVALID_ARG;
    }
#ifdef CONFIG_MOTOR_INTERLOCK_ACTIVE_HIGH
    const int inactive_level = 0;
#else
    const int inactive_level = 1;
#endif
    // Load the safe output latch before changing the pad direction, preventing
    // an active-low relay/load-switch pulse during boot.
    esp_err_t error =
        gpio_set_level(CONFIG_MOTOR_INTERLOCK_GPIO, inactive_level);
    if (error != ESP_OK) return error;
    gpio_config_t interlock_gpio_config = {
        .pin_bit_mask = 1ULL << CONFIG_MOTOR_INTERLOCK_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    error = gpio_config(&interlock_gpio_config);
    if (error != ESP_OK) return error;
#endif

    SemaphoreHandle_t stop_mutex = xSemaphoreCreateRecursiveMutex();
    if (!stop_mutex) return ESP_ERR_NO_MEM;

    portENTER_CRITICAL(&s_state_lock);
    if (!s_stop_mutex) {
        s_stop_mutex = stop_mutex;
        stop_mutex = NULL;
    }
    portEXIT_CRITICAL(&s_state_lock);
    esp_err_t drive_error = interlock_drive(false);
    if (drive_error == ESP_OK) {
        portENTER_CRITICAL(&s_state_lock);
        s_initialized = true;
        portEXIT_CRITICAL(&s_state_lock);
    }
    if (stop_mutex) vSemaphoreDelete(stop_mutex);
    return drive_error;
}

esp_err_t safety_supervisor_configure(
    const safety_supervisor_callbacks_t *callbacks)
{
    portENTER_CRITICAL(&s_state_lock);
    if (!s_initialized || s_started || s_starting) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_callbacks = callbacks ? *callbacks : (safety_supervisor_callbacks_t) {0};
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t safety_supervisor_configure_watchdog(void)
{
    portENTER_CRITICAL(&s_state_lock);
    bool initialized = s_initialized;
    portEXIT_CRITICAL(&s_state_lock);
    if (!initialized) return ESP_ERR_INVALID_STATE;

#ifdef CONFIG_SUPERVISOR_ENABLED
    esp_task_wdt_config_t config = {
        .timeout_ms = CONFIG_SUPERVISOR_TASK_WDT_MS,
        .idle_core_mask = (1U << portNUM_PROCESSORS) - 1U,
        .trigger_panic = true,
    };
    esp_err_t error = esp_task_wdt_reconfigure(&config);
    if (error == ESP_ERR_INVALID_STATE) {
        error = esp_task_wdt_init(&config);
    }
    if (error != ESP_OK) return error;
#endif

    portENTER_CRITICAL(&s_state_lock);
    s_watchdog_configured = true;
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

static bool heartbeat_expired(int64_t now, int64_t heartbeat,
                              int64_t timeout_us)
{
    portENTER_CRITICAL(&s_state_lock);
    int64_t started = s_supervision_started_us;
    portEXIT_CRITICAL(&s_state_lock);
    if (started <= 0 || now <= started) return false;
    int64_t reference = heartbeat > started ? heartbeat : started;
    return now > reference && now - reference > timeout_us;
}

static bool restart_attempt_due(int64_t now)
{
    bool due;
    portENTER_CRITICAL(&s_state_lock);
    due = s_last_restart_attempt_us == 0 ||
          (now > s_last_restart_attempt_us &&
           now - s_last_restart_attempt_us >=
               (int64_t)SUPERVISOR_RESTART_RETRY_MS * 1000);
    if (due) s_last_restart_attempt_us = now;
    portEXIT_CRITICAL(&s_state_lock);
    return due;
}

static bool restart_after_fail_safe_stop(const char *reason)
{
    esp_err_t stop_error = stop_all_internal(reason, true,
                                             STOP_ACTION_GLOBAL, NULL);
    if (stop_error != ESP_OK) {
        ESP_LOGE(TAG,
                 "Restart suppressed after '%s': safe stop/zero commit failed: %s",
                 reason, esp_err_to_name(stop_error));
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return true;
}

static void supervisor_task(void *argument)
{
    (void)argument;
    int64_t supervision_started = esp_timer_get_time();
    portENTER_CRITICAL(&s_state_lock);
    s_supervision_started_us = supervision_started;
    portEXIT_CRITICAL(&s_state_lock);
#ifdef CONFIG_SUPERVISOR_ENABLED
    esp_err_t watchdog_error = esp_task_wdt_add(NULL);
    if (watchdog_error != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register supervisor with task watchdog: %s",
                 esp_err_to_name(watchdog_error));
        (void)restart_after_fail_safe_stop(
            "supervisor watchdog registration failed");
    }
#endif

    for (;;) {
        int64_t now = esp_timer_get_time();
#ifdef CONFIG_SUPERVISOR_ENABLED
        int64_t ramp_heartbeat = motor_control_ramp_heartbeat_us();
        if (heartbeat_expired(
                now, ramp_heartbeat,
                (int64_t)CONFIG_RAMP_HEARTBEAT_TIMEOUT_MS * 1000)) {
            portENTER_CRITICAL(&s_state_lock);
            s_ramp_liveness_fault = true;
            portEXIT_CRITICAL(&s_state_lock);
            if (restart_attempt_due(now)) {
                (void)restart_after_fail_safe_stop(
                    "motor ramp heartbeat expired");
            }
        }

#ifdef CONFIG_SCHEDULE_ENABLED
        int64_t scheduler_heartbeat = scheduler_get_heartbeat_us();
        if (heartbeat_expired(
                now, scheduler_heartbeat,
                (int64_t)CONFIG_SUPERVISOR_AUX_HEARTBEAT_TIMEOUT_MS * 1000)) {
            portENTER_CRITICAL(&s_state_lock);
            s_scheduler_liveness_fault = true;
            portEXIT_CRITICAL(&s_state_lock);
            if (restart_attempt_due(now)) {
                (void)restart_after_fail_safe_stop(
                    "scheduler heartbeat expired");
            }
        }
#endif

#ifdef CONFIG_TACH_FEEDBACK_ENABLED
        int64_t tach_timeout_ms = CONFIG_SUPERVISOR_AUX_HEARTBEAT_TIMEOUT_MS;
        int64_t tach_minimum_ms =
            (int64_t)CONFIG_TACH_SAMPLE_MS * 2 + 1000;
        if (tach_timeout_ms < tach_minimum_ms) {
            tach_timeout_ms = tach_minimum_ms;
        }
        int64_t tach_heartbeat = tach_monitor_get_heartbeat_us();
        if (heartbeat_expired(now, tach_heartbeat,
                              tach_timeout_ms * 1000)) {
            portENTER_CRITICAL(&s_state_lock);
            s_tach_liveness_fault = true;
            portEXIT_CRITICAL(&s_state_lock);
            if (restart_attempt_due(now)) {
                (void)restart_after_fail_safe_stop(
                    "tach monitor heartbeat expired");
            }
        }

        safety_supervisor_callbacks_t callbacks;
        portENTER_CRITICAL(&s_state_lock);
        callbacks = s_callbacks;
        portEXIT_CRITICAL(&s_state_lock);
        safety_supervisor_tach_action_health_t action_health = {0};
        bool action_health_valid = callbacks.read_tach_action_health &&
            callbacks.read_tach_action_health(callbacks.callback_context,
                                              &action_health);
        int64_t action_timeout_ms =
            CONFIG_SUPERVISOR_AUX_HEARTBEAT_TIMEOUT_MS;
        if (action_timeout_ms < SAFETY_TACH_ACTION_TIMEOUT_MS) {
            action_timeout_ms = SAFETY_TACH_ACTION_TIMEOUT_MS;
        }
        bool action_heartbeat_expired = action_health_valid &&
            heartbeat_expired(now, action_health.heartbeat_us,
                              action_timeout_ms * 1000);
        bool action_age_expired = action_health_valid &&
            action_health.oldest_action_since_us > 0 &&
            now > action_health.oldest_action_since_us &&
            now - action_health.oldest_action_since_us >
                action_timeout_ms * 1000;
        if (!action_health_valid || !action_health.ready ||
            action_heartbeat_expired || action_age_expired) {
            portENTER_CRITICAL(&s_state_lock);
            s_tach_liveness_fault = true;
            portEXIT_CRITICAL(&s_state_lock);
            if (restart_attempt_due(now)) {
                (void)restart_after_fail_safe_stop(
                    "tach safety-action task expired");
            }
        }
#endif
#endif

        // Consume the edge before latching the durable RMT inhibit. Further
        // failures can raise a new edge, while command authorization observes
        // the inhibit atomically before any blocking RMT stop work begins.
        if (motor_control_take_rmt_fault_pending()) {
#ifdef CONFIG_RMT_FAILURE_REBOOT
            portENTER_CRITICAL(&s_state_lock);
            s_rmt_reboot_pending = true;
            portEXIT_CRITICAL(&s_state_lock);
#else
            (void)stop_all_internal("repeated RMT failures", false,
                                    STOP_ACTION_RMT, NULL);
#endif
        }

#ifdef CONFIG_RMT_FAILURE_REBOOT
        portENTER_CRITICAL(&s_state_lock);
        bool rmt_reboot_pending = s_rmt_reboot_pending;
        portEXIT_CRITICAL(&s_state_lock);
        if (rmt_reboot_pending && restart_attempt_due(now)) {
            stop_outcome_t outcome;
            (void)stop_all_internal("repeated RMT failures", true,
                                    STOP_ACTION_RMT, &outcome);
            // A reboot is the recovery mechanism when an infinite RMT stream
            // itself cannot be stopped. Requiring motor_error == ESP_OK here
            // would suppress the one action capable of resetting that
            // peripheral. Reboot once both sources which could re-authorize
            // motion after reset have durably committed their safe state.
            if (outcome.automatic_committed &&
                outcome.persistence_committed) {
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            }
            ESP_LOGE(TAG,
                     "RMT-fault reboot deferred: automatic=%s targets=%s "
                     "stop_result=%s",
                     outcome.automatic_committed ? "committed" : "failed",
                     outcome.persistence_committed ? "committed" : "failed",
                     esp_err_to_name(outcome.result));
        }
#endif

        mqtt_manager_health_t mqtt_health = mqtt_manager_health_snapshot();
        bool dispatch_heartbeat_expired = heartbeat_expired(
            now, mqtt_health.command_dispatch_heartbeat_us,
            (int64_t)SAFETY_MQTT_DISPATCH_TIMEOUT_MS * 1000);
        bool dispatch_age_expired =
            mqtt_health.oldest_command_since_us > 0 &&
            now > mqtt_health.oldest_command_since_us &&
            now - mqtt_health.oldest_command_since_us >
                (int64_t)SAFETY_MQTT_DISPATCH_TIMEOUT_MS * 1000;
        if (!mqtt_health.command_dispatch_ready ||
            dispatch_heartbeat_expired || dispatch_age_expired) {
            portENTER_CRITICAL(&s_state_lock);
            s_mqtt_dispatch_liveness_fault = true;
            portEXIT_CRITICAL(&s_state_lock);
            if (restart_attempt_due(now)) {
                (void)restart_after_fail_safe_stop(
                    "MQTT command dispatcher expired");
            }
        }

        bool command_overflow = false;
        portENTER_CRITICAL(&s_state_lock);
        if (mqtt_health.command_queue_drops !=
            s_observed_mqtt_command_queue_drops) {
            s_observed_mqtt_command_queue_drops =
                mqtt_health.command_queue_drops;
            s_mqtt_command_overflow_fault = true;
            command_overflow = true;
        }
        portEXIT_CRITICAL(&s_state_lock);
        if (command_overflow) {
            // Queue saturation means MQTT command ordering is no longer
            // trustworthy (the missed item could have been OFF). Latch the
            // communication source off until a fresh command arrives through
            // a clean, fully subscribed session.
            (void)stop_all_internal("MQTT command queue overflow", true,
                                    STOP_ACTION_COMMUNICATION, NULL);
        }

#ifdef CONFIG_COMM_LOSS_STOP_AFTER_LEASE
        int64_t last_ack_us = mqtt_health.last_ack_us;
        int64_t lease_us =
            (int64_t)CONFIG_COMMUNICATION_LEASE_MS * 1000;
        if (last_ack_us > 0 && now > last_ack_us &&
            now - last_ack_us > lease_us) {
            motor_control_health_snapshot_t motor_health;
            if (motor_control_get_health_snapshot(&motor_health) &&
                !motor_health.communication_inhibited) {
                // This API publishes the communication inhibit under the same
                // motor lock used by manual-command authorization, closing the
                // stale-authorization window before its synchronous stop.
                (void)stop_all_internal("MQTT communication lease expired",
                                        true,
                                        STOP_ACTION_COMMUNICATION, NULL);
            }
        }
#endif

#ifdef CONFIG_SUPERVISOR_ENABLED
        (void)esp_task_wdt_reset();
#endif
        // Record liveness only after this complete supervision pass.
        portENTER_CRITICAL(&s_state_lock);
        s_heartbeat_us = esp_timer_get_time();
        portEXIT_CRITICAL(&s_state_lock);
        vTaskDelay(pdMS_TO_TICKS(SUPERVISOR_PERIOD_MS));
    }
}

esp_err_t safety_supervisor_start(void)
{
    portENTER_CRITICAL(&s_state_lock);
    if (!s_initialized) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_started) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_OK;
    }
    if (s_starting) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
#ifdef CONFIG_SUPERVISOR_ENABLED
    if (!s_watchdog_configured) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
#endif
    s_starting = true;
    portEXIT_CRITICAL(&s_state_lock);

    TaskHandle_t task = NULL;
    BaseType_t created = xTaskCreate(supervisor_task, "supervisor",
                                     SUPERVISOR_TASK_STACK, NULL,
                                     SUPERVISOR_TASK_PRIORITY, &task);
    portENTER_CRITICAL(&s_state_lock);
    s_starting = false;
    if (created == pdPASS) {
        s_supervisor_task = task;
        s_started = true;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool safety_supervisor_enable_output_if_safe(void)
{
    portENTER_CRITICAL(&s_state_lock);
    SemaphoreHandle_t stop_mutex = s_stop_mutex;
    portEXIT_CRITICAL(&s_state_lock);
    if (stop_mutex) {
        xSemaphoreTakeRecursive(stop_mutex, portMAX_DELAY);
    }

    uint32_t generation;
    portENTER_CRITICAL(&s_state_lock);
    bool can_attempt = s_initialized && s_stop_depth == 0;
    generation = s_safety_generation;
    portEXIT_CRITICAL(&s_state_lock);
    if (!can_attempt || !motor_control_output_allowed()) {
        if (stop_mutex) xSemaphoreGiveRecursive(stop_mutex);
        return false;
    }

    portENTER_CRITICAL(&s_state_lock);
    bool unchanged = s_initialized && s_stop_depth == 0 &&
                     generation == s_safety_generation;
    portEXIT_CRITICAL(&s_state_lock);
    esp_err_t error = unchanged ? interlock_drive(true) :
                                  ESP_ERR_INVALID_STATE;
    if (error != ESP_OK) {
        if (stop_mutex) xSemaphoreGiveRecursive(stop_mutex);
        return false;
    }

    // A motor-side inhibit can be raised independently of this module. Close
    // the check-to-enable interval immediately if that happened.
    if (!motor_control_output_allowed()) {
        (void)interlock_drive(false);
        if (stop_mutex) xSemaphoreGiveRecursive(stop_mutex);
        return false;
    }
    if (stop_mutex) xSemaphoreGiveRecursive(stop_mutex);
    return true;
}

esp_err_t safety_supervisor_output_command_begin(TickType_t timeout_ticks)
{
    portENTER_CRITICAL(&s_state_lock);
    SemaphoreHandle_t stop_mutex = s_stop_mutex;
    bool initialized = s_initialized;
    portEXIT_CRITICAL(&s_state_lock);
    if (!initialized || !stop_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTakeRecursive(stop_mutex, timeout_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    // A stop claims stop_depth before waiting for this mutex. Therefore an
    // output command either owns the guard before the claim (and the stop runs
    // immediately after it), or observes the claim here and is rejected.
    portENTER_CRITICAL(&s_state_lock);
    bool allowed = s_stop_depth == 0;
    portEXIT_CRITICAL(&s_state_lock);
    if (!allowed) {
        xSemaphoreGiveRecursive(stop_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

void safety_supervisor_output_command_end(void)
{
    portENTER_CRITICAL(&s_state_lock);
    SemaphoreHandle_t stop_mutex = s_stop_mutex;
    portEXIT_CRITICAL(&s_state_lock);
    if (stop_mutex) xSemaphoreGiveRecursive(stop_mutex);
}

esp_err_t safety_supervisor_stop_all(const char *reason,
                                     bool clear_manual_targets,
                                     bool latch_global)
{
    return stop_all_internal(reason, clear_manual_targets,
                             latch_global ? STOP_ACTION_GLOBAL :
                                            STOP_ACTION_PLAIN,
                             NULL);
}

esp_err_t safety_supervisor_prepare_for_ota(bool clear_manual_targets)
{
    return stop_all_internal("OTA preparation", clear_manual_targets,
                             STOP_ACTION_VERIFIED, NULL);
}

esp_err_t safety_supervisor_release_maintenance(void)
{
    return motor_control_clear_maintenance_inhibit();
}

esp_err_t safety_supervisor_stop_for_tach_all(
    const char *reason, bool clear_manual_targets)
{
    return stop_all_internal(reason, clear_manual_targets,
                             STOP_ACTION_TACH_ALL, NULL);
}

bool safety_supervisor_interlock_enabled(void)
{
    portENTER_CRITICAL(&s_state_lock);
    bool enabled = s_interlock_enabled;
    portEXIT_CRITICAL(&s_state_lock);
    return enabled;
}

bool safety_supervisor_interlock_present(void)
{
#ifdef CONFIG_MOTOR_INTERLOCK_ENABLED
    return true;
#else
    return false;
#endif
}

void safety_supervisor_set_boot_health(safety_boot_health_t health)
{
    if (health < SAFETY_BOOT_INITIALIZING ||
        health > SAFETY_BOOT_ROLLBACK_FAILED_SAFE) {
        return;
    }
    portENTER_CRITICAL(&s_state_lock);
    s_boot_health = health;
    portEXIT_CRITICAL(&s_state_lock);
}

const char *safety_supervisor_boot_health_name(safety_boot_health_t health)
{
    switch (health) {
        case SAFETY_BOOT_INITIALIZING: return "initializing";
        case SAFETY_BOOT_HEALTHY: return "healthy";
        case SAFETY_BOOT_HEALTHY_OFFLINE_CONFIG:
            return "healthy_offline_config";
        case SAFETY_BOOT_HEALTHY_OFFLINE_WIFI:
            return "healthy_offline_wifi";
        case SAFETY_BOOT_FAILED_SAFE: return "failed_safe";
        case SAFETY_BOOT_ROLLBACK_FAILED_SAFE:
            return "rollback_failed_safe";
        default: return "unknown";
    }
}

safety_supervisor_health_t safety_supervisor_health_snapshot(void)
{
    safety_supervisor_health_t snapshot = {0};
    TaskHandle_t task;
    safety_supervisor_callbacks_t callbacks;
    portENTER_CRITICAL(&s_state_lock);
    snapshot.initialized = s_initialized;
#ifdef CONFIG_MOTOR_INTERLOCK_ENABLED
    snapshot.hardware_interlock_present = true;
#endif
    snapshot.interlock_enabled = s_interlock_enabled;
    snapshot.supervisor_started = s_started;
    snapshot.watchdog_configured = s_watchdog_configured;
    snapshot.stop_in_progress = s_stop_depth > 0;
    snapshot.boot_health = s_boot_health;
    snapshot.heartbeat_us = s_heartbeat_us;
    snapshot.stop_count = s_stop_count;
    snapshot.last_stop_result = s_last_stop_result;
    snapshot.ramp_liveness_fault = s_ramp_liveness_fault;
    snapshot.scheduler_liveness_fault = s_scheduler_liveness_fault;
    snapshot.tach_liveness_fault = s_tach_liveness_fault;
    snapshot.mqtt_command_overflow_fault =
        s_mqtt_command_overflow_fault;
    snapshot.mqtt_dispatch_liveness_fault =
        s_mqtt_dispatch_liveness_fault;
    memcpy(snapshot.last_stop_reason, s_last_stop_reason,
           sizeof(snapshot.last_stop_reason));
    task = s_supervisor_task;
    callbacks = s_callbacks;
    portEXIT_CRITICAL(&s_state_lock);

    motor_control_health_snapshot_t motor_health;
    if (motor_control_get_health_snapshot(&motor_health)) {
        snapshot.stop_in_progress = snapshot.stop_in_progress ||
                                    motor_health.stop_in_progress;
        snapshot.global_safety_latched = motor_health.global_inhibited;
        snapshot.communication_failsafe_active =
            motor_health.communication_inhibited;
        snapshot.rmt_inhibited = motor_health.rmt_inhibited;
        snapshot.maintenance_inhibited = motor_health.maintenance_inhibited;
    }
    if (task) {
        snapshot.stack_words =
            (uint32_t)uxTaskGetStackHighWaterMark(task);
    }
#ifdef CONFIG_TACH_FEEDBACK_ENABLED
    safety_supervisor_tach_action_health_t action_health = {0};
    if (callbacks.read_tach_action_health &&
        callbacks.read_tach_action_health(callbacks.callback_context,
                                          &action_health)) {
        snapshot.tach_action_ready = action_health.ready;
        snapshot.tach_action_pending =
            action_health.oldest_action_since_us > 0;
        int64_t now = esp_timer_get_time();
        snapshot.tach_action_age_ms =
            action_health.oldest_action_since_us > 0 &&
                    now > action_health.oldest_action_since_us
                ? (now - action_health.oldest_action_since_us) / 1000
                : 0;
        snapshot.tach_action_stack_words = action_health.stack_words;
    }
#else
    (void)callbacks;
    // No worker is required when tach feedback is compiled out.
    snapshot.tach_action_ready = true;
#endif
    return snapshot;
}

int64_t safety_supervisor_heartbeat_us(void)
{
    portENTER_CRITICAL(&s_state_lock);
    int64_t heartbeat = s_heartbeat_us;
    portEXIT_CRITICAL(&s_state_lock);
    return heartbeat;
}

uint32_t safety_supervisor_stack_words(void)
{
    portENTER_CRITICAL(&s_state_lock);
    TaskHandle_t task = s_supervisor_task;
    portEXIT_CRITICAL(&s_state_lock);
    return task ? (uint32_t)uxTaskGetStackHighWaterMark(task) : 0;
}
