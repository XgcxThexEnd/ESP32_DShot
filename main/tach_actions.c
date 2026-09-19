#include "tach_actions.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fan_state_store.h"
#include "motor_control.h"
#include "scheduler.h"
#include "state_publisher.h"

#define TACH_ACTION_TASK_STACK 6144
#define TACH_ACTION_TASK_PRIORITY 7
#define TACH_ACTION_PERSIST_TIMEOUT_MS 3000
#define TACH_CLEAR_SERIALIZE_WAIT_MS 10000
#ifdef CONFIG_TACH_FEEDBACK_ENABLED
static const char *TAG = "tach_actions";
#endif

#ifdef CONFIG_TACH_FEEDBACK_ENABLED
static SemaphoreHandle_t s_tach_action_mutex;
static TaskHandle_t s_tach_action_task;
static portMUX_TYPE s_tach_pending_lock = portMUX_INITIALIZER_UNLOCKED;
typedef struct {
    bool pending;
    int64_t queued_at_us;
    tach_monitor_stall_event_t event;
} pending_tach_action_t;
static pending_tach_action_t s_pending_tach_actions[APP_CONFIG_MAX_FANS];
static bool s_tach_action_ready;
static int64_t s_tach_action_heartbeat_us;
static int64_t s_tach_action_in_flight_since_us;
#endif

#ifdef CONFIG_TACH_FEEDBACK_ENABLED
bool tach_actions_read_motor_snapshot(
    int local_fan, tach_monitor_motor_snapshot_t *out_snapshot,
    void *context)
{
    (void)context;
    if (!out_snapshot) return false;
    motor_control_fan_snapshot_t motor;
    if (!motor_control_get_fan_snapshot(local_fan, &motor)) return false;
    *out_snapshot = (tach_monitor_motor_snapshot_t) {
        .applied_pct = motor.applied_pct,
        .nonzero_applied_since_us = motor.nonzero_applied_since_us,
    };
    return true;
}

bool tach_actions_read_health(
    void *context, safety_supervisor_tach_action_health_t *out_health)
{
    (void)context;
    if (!out_health) return false;

    TaskHandle_t action_task;
    int64_t oldest_action_since_us;
    portENTER_CRITICAL(&s_tach_pending_lock);
    *out_health = (safety_supervisor_tach_action_health_t) {
        .ready = s_tach_action_ready,
        .heartbeat_us = s_tach_action_heartbeat_us,
        .oldest_action_since_us = s_tach_action_in_flight_since_us,
    };
    oldest_action_since_us = s_tach_action_in_flight_since_us;
    for (int i = 0; i < APP_CONFIG_MAX_FANS; ++i) {
        int64_t queued_at_us = s_pending_tach_actions[i].pending
                                   ? s_pending_tach_actions[i].queued_at_us
                                   : 0;
        if (queued_at_us > 0 &&
            (oldest_action_since_us == 0 ||
             queued_at_us < oldest_action_since_us)) {
            oldest_action_since_us = queued_at_us;
        }
    }
    out_health->oldest_action_since_us = oldest_action_since_us;
    action_task = s_tach_action_task;
    portEXIT_CRITICAL(&s_tach_pending_lock);
    if (action_task) {
        out_health->stack_bytes =
            (uint32_t)uxTaskGetStackHighWaterMark(action_task);
    }
    return true;
}

static void tach_action_mark_complete(void)
{
    portENTER_CRITICAL(&s_tach_pending_lock);
    s_tach_action_in_flight_since_us = 0;
    s_tach_action_heartbeat_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_tach_pending_lock);
}

static const char *tach_fault_reason(tach_monitor_fault_cause_t cause)
{
    return cause == TACH_MONITOR_FAULT_SENSOR_INVALID
               ? "tachometer feedback invalid"
               : "tachometer stall";
}

static void tach_escalate_to_global_stop(const char *reason,
                                         esp_err_t initiating_error)
{
    ESP_LOGE(TAG, "%s; escalating tach response to global stop: %s",
             reason, esp_err_to_name(initiating_error));
    esp_err_t stop_error = safety_supervisor_stop_all(reason, true, true);
    if (stop_error != ESP_OK) {
        ESP_LOGE(TAG, "Escalated tach safety transaction failed: %s",
                 esp_err_to_name(stop_error));
    }
}

static bool tach_event_is_current(const tach_monitor_stall_event_t *event)
{
    tach_monitor_snapshot_t snapshot;
    return event &&
           tach_monitor_get_snapshot(event->local_fan, &snapshot) == ESP_OK &&
           snapshot.stall_alarm && snapshot.fault_cause == event->cause &&
           snapshot.fault_generation == event->fault_generation;
}

static void tach_action_task(void *argument)
{
    (void)argument;
    portENTER_CRITICAL(&s_tach_pending_lock);
    s_tach_action_ready = true;
    s_tach_action_heartbeat_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_tach_pending_lock);

    for (;;) {
        uint32_t notified_fans = 0;
        (void)xTaskNotifyWait(
            0, UINT32_MAX, &notified_fans,
            pdMS_TO_TICKS(TACH_ACTION_HEARTBEAT_MS));

        for (int local_fan = 0; local_fan < APP_CONFIG_MAX_FANS;
             ++local_fan) {
            if ((notified_fans & (1UL << local_fan)) == 0) continue;

            tach_monitor_stall_event_t event;
            bool pending;
            portENTER_CRITICAL(&s_tach_pending_lock);
            pending = s_pending_tach_actions[local_fan].pending;
            event = s_pending_tach_actions[local_fan].event;
            s_pending_tach_actions[local_fan].pending = false;
            if (pending) {
                s_tach_action_in_flight_since_us =
                    s_pending_tach_actions[local_fan].queued_at_us;
            }
            portEXIT_CRITICAL(&s_tach_pending_lock);
            if (!pending) continue;

            if (xSemaphoreTake(s_tach_action_mutex, portMAX_DELAY) != pdTRUE) {
                tach_escalate_to_global_stop(
                    "tach action serialization failed", ESP_ERR_TIMEOUT);
                tach_action_mark_complete();
                continue;
            }

            // CLEAR can overtake callback dispatch after the monitor publishes
            // a fault. Revalidate the generation while serialized with CLEAR
            // so that delayed work cannot re-latch an alarm already cleared by
            // the operator.
            if (!tach_event_is_current(&event)) {
                xSemaphoreGive(s_tach_action_mutex);
                tach_action_mark_complete();
                continue;
            }

            if (event.action == TACH_MONITOR_STALL_STOP_ALL) {
                esp_err_t err = safety_supervisor_stop_for_tach_all(
                    tach_fault_reason(event.cause), true);
                if (err != ESP_OK) {
                    tach_escalate_to_global_stop(
                        "tach stop-all transaction failed", err);
                }
            } else if (event.action == TACH_MONITOR_STALL_STOP_FAN) {
                esp_err_t motor_error = motor_control_set_tach_inhibit(
                    event.local_fan, true);
                esp_err_t scheduler_error = ESP_OK;
                esp_err_t persistence_error = ESP_OK;
                if (motor_error == ESP_OK) {
                    scheduler_error = scheduler_cancel_fan(event.local_fan);
                }
                if (motor_error == ESP_OK && scheduler_error == ESP_OK) {
                    // A debounced save is not sufficient here: a reset shortly
                    // after the stall must not restore the pre-fault target.
                    persistence_error = fan_state_store_save_sync(
                        pdMS_TO_TICKS(TACH_ACTION_PERSIST_TIMEOUT_MS));
                }
                if (motor_error != ESP_OK || scheduler_error != ESP_OK ||
                    persistence_error != ESP_OK) {
                    esp_err_t first_error = motor_error != ESP_OK
                                                ? motor_error
                                                : scheduler_error != ESP_OK
                                                      ? scheduler_error
                                                      : persistence_error;
                    tach_escalate_to_global_stop(
                        "per-fan tach stop could not be committed",
                        first_error);
                } else {
                    state_publisher_request_fan(event.local_fan, true);
                }
            } else {
                state_publisher_request_fan(event.local_fan, true);
            }
            xSemaphoreGive(s_tach_action_mutex);
            tach_action_mark_complete();
        }
        portENTER_CRITICAL(&s_tach_pending_lock);
        s_tach_action_heartbeat_us = esp_timer_get_time();
        portEXIT_CRITICAL(&s_tach_pending_lock);
    }
}

void tach_actions_confirmed_stall(const tach_monitor_stall_event_t *event,
                                 void *context)
{
    (void)context;
    if (!event || event->local_fan < 0 ||
        event->local_fan >= APP_CONFIG_MAX_FANS) {
        return;
    }

    TaskHandle_t action_task = s_tach_action_task;
    if (!action_task) {
        // Start order makes this unreachable, but a missing dispatcher must
        // fail closed rather than silently discarding a confirmed fault.
        tach_escalate_to_global_stop(
            "tach action dispatcher unavailable", ESP_ERR_INVALID_STATE);
        return;
    }

    // Keep the PCNT sampling task bounded. A single pending slot per fan is
    // lossless because a latched fault cannot emit another event until CLEAR;
    // if CLEAR/new-fault races this copy, the newer generation wins.
    portENTER_CRITICAL(&s_tach_pending_lock);
    s_pending_tach_actions[event->local_fan].event = *event;
    s_pending_tach_actions[event->local_fan].queued_at_us =
        esp_timer_get_time();
    s_pending_tach_actions[event->local_fan].pending = true;
    portEXIT_CRITICAL(&s_tach_pending_lock);
    (void)xTaskNotify(action_task, 1UL << event->local_fan, eSetBits);
}
#endif

esp_err_t tach_actions_clear_begin(void)
{
#ifdef CONFIG_TACH_FEEDBACK_ENABLED
    if (!s_tach_action_mutex) return ESP_ERR_INVALID_STATE;
    return xSemaphoreTake(
               s_tach_action_mutex,
               pdMS_TO_TICKS(TACH_CLEAR_SERIALIZE_WAIT_MS)) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t tach_actions_clear_locked(int local_fan)
{
#ifdef CONFIG_TACH_FEEDBACK_ENABLED
    esp_err_t tach_err = tach_monitor_clear_alarm(local_fan);
    esp_err_t motor_err = tach_err == ESP_OK
                              ? motor_control_set_tach_inhibit(local_fan, false)
                              : tach_err;
    return tach_err != ESP_OK ? tach_err : motor_err;
#else
    (void)local_fan;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void tach_actions_clear_end(void)
{
#ifdef CONFIG_TACH_FEEDBACK_ENABLED
    if (s_tach_action_mutex) xSemaphoreGive(s_tach_action_mutex);
#endif
}

esp_err_t tach_actions_init(void)
{
#ifdef CONFIG_TACH_FEEDBACK_ENABLED
    if (s_tach_action_mutex) return ESP_ERR_INVALID_STATE;
    s_tach_action_mutex = xSemaphoreCreateMutex();
    return s_tach_action_mutex ? ESP_OK : ESP_ERR_NO_MEM;
#else
    return ESP_OK;
#endif
}

esp_err_t tach_actions_start(void)
{
#ifdef CONFIG_TACH_FEEDBACK_ENABLED
    if (!s_tach_action_mutex) return ESP_ERR_INVALID_STATE;
    if (s_tach_action_task) return ESP_OK;
    return xTaskCreate(tach_action_task, "tach_action", TACH_ACTION_TASK_STACK,
                       NULL, TACH_ACTION_TASK_PRIORITY, &s_tach_action_task) == pdPASS
               ? ESP_OK : ESP_ERR_NO_MEM;
#else
    return ESP_OK;
#endif
}
