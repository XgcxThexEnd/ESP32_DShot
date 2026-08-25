/*
 * ESP-IDF greenhouse controller composition root.
 *
 * This file preserves the safety-critical boot order and wires the focused
 * Wi-Fi, MQTT, motor, tach, scheduler, OTA, publishing, and supervisor modules.
 */

#include <string.h>
#include <stdio.h>
#include <stddef.h>

#include "sdkconfig.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "This firmware currently supports only ESP32-S3 targets"
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "command_router.h"
#include "fan_state_store.h"
#include "motor_control.h"
#include "mqtt_manager.h"
#include "ota_manager.h"
#include "safety_supervisor.h"
#include "scheduler.h"
#include "state_publisher.h"
#include "tach_monitor.h"
#include "wifi_manager.h"

// Wi-Fi/MQTT credentials come from menuconfig (Kconfig) so secrets
// stay out of source control. Hard fallbacks below only apply when
// the Kconfig options are absent entirely (e.g. stale sdkconfig).
#ifndef CONFIG_WIFI_SSID
#define CONFIG_WIFI_SSID ""
#endif
#ifndef CONFIG_WIFI_PASS
#define CONFIG_WIFI_PASS ""
#endif
#ifndef CONFIG_MQTT_BROKER_URI
#define CONFIG_MQTT_BROKER_URI ""
#endif
#ifndef CONFIG_MQTT_USERNAME
#define CONFIG_MQTT_USERNAME   ""
#endif
#ifndef CONFIG_MQTT_PASSWORD
#define CONFIG_MQTT_PASSWORD   ""
#endif
#ifndef CONFIG_WIFI_STARTUP_GRACE_MS
#define CONFIG_WIFI_STARTUP_GRACE_MS 30000
#endif
#ifndef CONFIG_TLS_TIME_SYNC_TIMEOUT_MS
#define CONFIG_TLS_TIME_SYNC_TIMEOUT_MS 30000
#endif
#ifndef CONFIG_SUPERVISOR_TASK_WDT_MS
#define CONFIG_SUPERVISOR_TASK_WDT_MS 8000
#endif


#if defined(CONFIG_OTA_ENABLED) && !defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)
#error "CONFIG_OTA_ENABLED requires bootloader application rollback"
#endif

static const app_config_t *s_app_config;
static command_router_config_t s_command_router_config;
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

static const char *TAG = "greenhouse";

static void request_state_publish(int idx, bool full_retained);
static void publish_states(void);

// ---- Timing / sizing constants (formerly magic numbers) ----
#define WIFI_CONNECT_TIMEOUT_MS   CONFIG_WIFI_STARTUP_GRACE_MS
#define MOTOR_BOOT_SETTLE_MS      1500    // let ESCs see idle frames before arming
#define HEARTBEAT_PERIOD_MS       5000    // periodic retained state publish
#define ROLLBACK_PERSIST_TIMEOUT_MS 3000  // bound zero-target NVS acknowledgement
#define MQTT_COMMAND_COMMIT_WAIT_MS 1500  // bound session-fence contention
#define MQTT_DISPATCH_HEALTH_MAX_AGE_MS 1000
#define TACH_ACTION_TASK_STACK    6144
#define TACH_ACTION_TASK_PRIORITY 7
#define TACH_ACTION_HEARTBEAT_MS  250
#define TACH_CLEAR_SERIALIZE_WAIT_MS 10000

static bool reset_reason_allows_motion_restore(esp_reset_reason_t reason)
{
    // Only resets which are expected during normal operation may consume a
    // previously persisted nonzero request. A panic/watchdog/brownout-style
    // reset can interrupt a safety commit, so treating its old NVS state as an
    // authorization to move would reintroduce the condition that just failed.
    return reason == ESP_RST_POWERON || reason == ESP_RST_SW ||
           reason == ESP_RST_DEEPSLEEP;
}

static void motor_event_callback(void *context,
                                 const motor_control_event_t *event)
{
    (void)context;
    if (!event) return;
    if (event->local_fan >= 0) {
        request_state_publish(event->local_fan,
                              event->type != MOTOR_CONTROL_EVENT_APPLIED_CHANGED);
    }
}

static bool fan_state_snapshot_targets(void *context, uint8_t *targets,
                                       size_t target_count)
{
    (void)context;
    if (!targets || !s_app_config ||
        target_count != (size_t)s_app_config->fan_count) {
        return false;
    }
    for (int i = 0; i < s_app_config->fan_count; ++i) {
        motor_control_fan_snapshot_t snapshot;
        if (!motor_control_get_fan_snapshot(i, &snapshot)) return false;
        targets[i] = snapshot.manual_target_pct;
    }
    return true;
}

static bool fan_state_restore_targets(void *context, const uint8_t *targets,
                                      size_t target_count)
{
    (void)context;
    return motor_control_restore_all(targets, target_count) == ESP_OK;
}

static void scheduler_apply_target_callback(
    const scheduler_target_request_t *request,
    scheduler_target_result_t *result, void *context)
{
    (void)context;
    if (!request || !result) return;
    *result = (scheduler_target_result_t) {0};
    if (request->kind == SCHEDULER_TARGET_MANUAL) {
        motor_control_ack_t ack;
        if (motor_control_request_manual(request->local_fan,
                                         request->target_pct, &ack) == ESP_OK) {
            result->accepted = ack.accepted;
            result->manual_target_pct = request->target_pct;
            result->communication_inhibit_cleared =
                ack.communication_inhibit_cleared;
            result->accepted_command_count = ack.accepted_command_count;
            result->requested_pct = ack.requested_pct;
            result->applied_pct = ack.applied_pct;
        }
    } else if (request->kind == SCHEDULER_TARGET_START) {
        result->accepted = motor_control_request_scheduled(
                               request->local_fan, request->target_pct,
                               &result->manual_target_pct) == ESP_OK;
    } else if (request->kind == SCHEDULER_TARGET_RESTORE) {
        result->accepted = motor_control_request_automatic(
                               request->local_fan,
                               request->target_pct) == ESP_OK;
    }
}

static void scheduler_notify_callback(const scheduler_event_t *event,
                                      void *context)
{
    (void)context;
    if (!event) return;
    if (event->local_fan >= 0) {
        request_state_publish(event->local_fan, true);
    } else if (event->type == SCHEDULER_EVENT_INHIBITED_ALL) {
        publish_states();
    }
}

static esp_err_t safety_inhibit_automatic_callback(
    void *context, TickType_t timeout_ticks)
{
    (void)context;
    return scheduler_try_inhibit_all(timeout_ticks);
}

static esp_err_t safety_request_persistence_callback(
    void *context, TickType_t timeout_ticks)
{
    (void)context;
    return fan_state_store_save_sync(timeout_ticks);
}

static void safety_notify_state_callback(void *context)
{
    (void)context;
    publish_states();
}

#ifdef CONFIG_TACH_FEEDBACK_ENABLED
static bool tach_read_motor_snapshot(
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

static bool read_tach_action_health(
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
        out_health->stack_words =
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
                        pdMS_TO_TICKS(ROLLBACK_PERSIST_TIMEOUT_MS));
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
                    request_state_publish(event.local_fan, true);
                }
            } else {
                request_state_publish(event.local_fan, true);
            }
            xSemaphoreGive(s_tach_action_mutex);
            tach_action_mark_complete();
        }
        portENTER_CRITICAL(&s_tach_pending_lock);
        s_tach_action_heartbeat_us = esp_timer_get_time();
        portEXIT_CRITICAL(&s_tach_pending_lock);
    }
}

static void tach_confirmed_stall(const tach_monitor_stall_event_t *event,
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

static esp_err_t tach_clear_transaction_begin(void)
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

static esp_err_t clear_tach_alarm_locked(int local_fan)
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

static void tach_clear_transaction_end(void)
{
#ifdef CONFIG_TACH_FEEDBACK_ENABLED
    if (s_tach_action_mutex) xSemaphoreGive(s_tach_action_mutex);
#endif
}


static bool interlock_enable_if_safe(void)
{
    return safety_supervisor_enable_output_if_safe();
}

static bool safety_stop_all(const char *reason,
                            bool clear_manual_targets, bool latch_global)
{
    return safety_supervisor_stop_all(reason, clear_manual_targets,
                                      latch_global) == ESP_OK;
}


static void request_state_publish(int idx, bool full_retained)
{
    state_publisher_request_fan(idx, full_retained);
}

static void publish_states(void)
{
    state_publisher_request_all();
}

static void read_publisher_coordinator_health(
    void *context, state_publisher_coordinator_health_t *out_snapshot)
{
    (void)context;
    if (!out_snapshot) return;
    safety_supervisor_health_t safety =
        safety_supervisor_health_snapshot();
    (void)snprintf(
        out_snapshot->boot_health, sizeof(out_snapshot->boot_health), "%s",
        safety_supervisor_boot_health_name(safety.boot_health));
    out_snapshot->interlock_enabled = safety.interlock_enabled;
    out_snapshot->communication_failsafe_active =
        safety.communication_failsafe_active;
    out_snapshot->global_safety_latched = safety.global_safety_latched;
    out_snapshot->ramp_liveness_fault = safety.ramp_liveness_fault;
    out_snapshot->scheduler_liveness_fault =
        safety.scheduler_liveness_fault;
    out_snapshot->tach_liveness_fault = safety.tach_liveness_fault;
    out_snapshot->tach_action_ready = safety.tach_action_ready;
    out_snapshot->tach_action_pending = safety.tach_action_pending;
    out_snapshot->tach_action_age_ms = safety.tach_action_age_ms;
    out_snapshot->tach_action_stack_words =
        safety.tach_action_stack_words;
    out_snapshot->mqtt_command_overflow_fault =
        safety.mqtt_command_overflow_fault;
    out_snapshot->mqtt_dispatch_liveness_fault =
        safety.mqtt_dispatch_liveness_fault;
    out_snapshot->supervisor_stack_words = safety.stack_words;
}



static esp_err_t apply_manual_command(int local_fan, uint8_t target_pct,
                                      scheduler_target_result_t *result)
{
    if (!result) return ESP_ERR_INVALID_ARG;
#ifdef CONFIG_SCHEDULE_ENABLED
    return scheduler_apply_manual_target(local_fan, target_pct, result);
#else
    motor_control_ack_t ack;
    esp_err_t err = motor_control_request_manual(local_fan, target_pct, &ack);
    *result = (scheduler_target_result_t) {
        .accepted = err == ESP_OK && ack.accepted,
        .manual_target_pct = target_pct,
        .communication_inhibit_cleared = ack.communication_inhibit_cleared,
        .accepted_command_count = ack.accepted_command_count,
        .requested_pct = ack.requested_pct,
        .applied_pct = ack.applied_pct,
    };
    return err;
#endif
}

static void acknowledge_manual_command(
    int local_fan, const scheduler_target_result_t *result)
{
    if (!result || !result->accepted) return;
    if (result->communication_inhibit_cleared) {
        ESP_LOGI(TAG,
                 "Communication-loss latch cleared by fresh manual command");
    }
    (void)state_publisher_publish_manual_ack(
        local_fan, result->accepted_command_count, result->requested_pct,
        result->applied_pct);
}

#ifdef CONFIG_OTA_ENABLED
static void ota_publish_status_callback(void *context, const char *status,
                                        bool retain)
{
    (void)context;
    (void)state_publisher_publish_ota_status(status, retain);
}

static bool ota_trusted_time_callback(void *context)
{
    (void)context;
    return wifi_manager_system_time_valid_for_tls();
}

static bool ota_prepare_callback(void *context)
{
    (void)context;
    esp_err_t err = safety_supervisor_prepare_for_ota(false);
    if (err != ESP_OK) return false;

    // OTA is a deliberate reboot which preserves the requested manual state.
    // Commit the latest snapshot now so an update cannot resurrect an older
    // debounced target if the download completes quickly.
    err = fan_state_store_save_sync(
        pdMS_TO_TICKS(ROLLBACK_PERSIST_TIMEOUT_MS));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA preparation could not persist fan targets: %s",
                 esp_err_to_name(err));
        if (safety_supervisor_release_maintenance() == ESP_OK) {
            (void)interlock_enable_if_safe();
        }
        return false;
    }
    motor_control_set_ramp_paused(true);
    return true;
}

static void ota_abort_callback(void *context)
{
    (void)context;
    if (safety_supervisor_release_maintenance() == ESP_OK) {
        (void)interlock_enable_if_safe();
        motor_control_set_ramp_paused(false);
    }
}
#endif

static void mqtt_on_message(void *context, const char *topic, const char *data,
                            size_t len, bool retained,
                            const mqtt_manager_command_session_t *session)
{
    (void)context;
    if (!topic || !data || !session || len != strlen(data)) {
        ESP_LOGW(TAG, "Rejected malformed MQTT command");
        return;
    }
    if (retained) {
        ESP_LOGW(TAG, "Rejected retained command on topic '%s'", topic);
        return;
    }
#ifdef CONFIG_OTA_ENABLED
    if (ota_manager_is_in_progress()) {
        ESP_LOGW(TAG, "Ignored command while OTA is in progress");
        return;
    }
#endif

    command_router_command_t command;
    command_router_result_t parse_result = command_router_parse(
        &s_command_router_config, topic, strlen(topic), data, len, false,
        &command);
    if (parse_result == COMMAND_ROUTER_NO_MATCH) return;
    if (parse_result != COMMAND_ROUTER_ACCEPTED) {
        ESP_LOGW(TAG, "Rejected MQTT command (router result %d) on '%s'",
                 (int)parse_result, topic);
        return;
    }
    ESP_LOGI(TAG, "MQTT RX topic='%s' payload_len=%zu", topic, len);

    switch (command.type) {
    case COMMAND_ROUTER_COMMAND_MANUAL_ON:
    case COMMAND_ROUTER_COMMAND_MANUAL_OFF:
    case COMMAND_ROUTER_COMMAND_MANUAL_PERCENTAGE: {
        uint8_t target = command.type == COMMAND_ROUTER_COMMAND_MANUAL_ON
                             ? (uint8_t)s_app_config->min_spin_pct
                             : command.type == COMMAND_ROUTER_COMMAND_MANUAL_OFF
                                   ? 0
                                   : command.value.percentage;
        scheduler_target_result_t result = {0};
        esp_err_t err = mqtt_manager_command_commit_begin(
            session, pdMS_TO_TICKS(MQTT_COMMAND_COMMIT_WAIT_MS));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Discarded fan%d command from a stale MQTT session",
                     command.fan_index);
            return;
        }
        err = safety_supervisor_output_command_begin(
            pdMS_TO_TICKS(MQTT_COMMAND_COMMIT_WAIT_MS));
        if (err == ESP_OK) {
            err = apply_manual_command(command.fan_local_index, target,
                                       &result);
            if (err == ESP_OK && result.accepted) {
                fan_state_store_request_save();
                // Enabling the output is part of both the session-authorized
                // mutation and the supervisor stop/output transaction.
                (void)interlock_enable_if_safe();
            }
            safety_supervisor_output_command_end();
        }
        mqtt_manager_command_commit_end();
        if (err != ESP_OK || !result.accepted) {
            ESP_LOGE(TAG, "fan%d command rejected by latched safety policy",
                     command.fan_index);
            return;
        }
        acknowledge_manual_command(command.fan_local_index, &result);
        request_state_publish(command.fan_local_index, true);
        break;
    }
    case COMMAND_ROUTER_COMMAND_TACH_CLEAR: {
        // The tach worker may legitimately hold its serialization mutex for a
        // multi-second safety stop. Wait for it before taking the short MQTT
        // generation fence, then revalidate immediately at the clear commit.
        esp_err_t err = tach_clear_transaction_begin();
        if (err == ESP_OK) {
            err = mqtt_manager_command_commit_begin(
                session, pdMS_TO_TICKS(MQTT_COMMAND_COMMIT_WAIT_MS));
            if (err == ESP_OK) {
                err = clear_tach_alarm_locked(command.fan_local_index);
                mqtt_manager_command_commit_end();
            }
            tach_clear_transaction_end();
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "fan%d tach alarm clear failed", command.fan_index);
            return;
        }
        request_state_publish(command.fan_local_index, true);
        break;
    }
    case COMMAND_ROUTER_COMMAND_SCHEDULE_OVERRIDE: {
        esp_err_t err = mqtt_manager_command_commit_begin(
            session, pdMS_TO_TICKS(MQTT_COMMAND_COMMIT_WAIT_MS));
        if (err == ESP_OK) {
            err = scheduler_set_override(
                command.value.schedule_override.inhibited);
            mqtt_manager_command_commit_end();
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Schedule override update failed");
        }
        break;
    }
    case COMMAND_ROUTER_COMMAND_SCHEDULE_SET: {
        esp_err_t err = mqtt_manager_command_commit_begin(
            session, pdMS_TO_TICKS(MQTT_COMMAND_COMMIT_WAIT_MS));
        if (err == ESP_OK) {
            err = scheduler_set_entry(
                command.fan_local_index, (int)command.schedule_slot,
                command.value.schedule_set.interval_min,
                command.value.schedule_set.duration_min,
                command.value.schedule_set.target_pct);
            mqtt_manager_command_commit_end();
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Schedule update failed for fan%d slot%u",
                     command.fan_index, command.schedule_slot);
        }
        break;
    }
    case COMMAND_ROUTER_COMMAND_SCHEDULE_DISABLE: {
        esp_err_t err = mqtt_manager_command_commit_begin(
            session, pdMS_TO_TICKS(MQTT_COMMAND_COMMIT_WAIT_MS));
        if (err == ESP_OK) {
            err = scheduler_disable_entry(command.fan_local_index,
                                          (int)command.schedule_slot);
            mqtt_manager_command_commit_end();
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Schedule disable failed for fan%d slot%u",
                     command.fan_index, command.schedule_slot);
        }
        break;
    }
    case COMMAND_ROUTER_COMMAND_SCHEDULE_GET: {
        scheduler_entry_snapshot_t snapshot;
        if (scheduler_get_entry(command.fan_local_index,
                                (int)command.schedule_slot,
                                &snapshot) != ESP_OK) {
            ESP_LOGW(TAG, "Schedule read failed for fan%d slot%u",
                     command.fan_index, command.schedule_slot);
            break;
        }
        (void)state_publisher_publish_schedule_entry(
            command.fan_local_index, command.schedule_slot, &snapshot);
        break;
    }
    case COMMAND_ROUTER_COMMAND_OTA_UPDATE: {
        esp_err_t fence_error = mqtt_manager_command_commit_begin(
            session, pdMS_TO_TICKS(MQTT_COMMAND_COMMIT_WAIT_MS));
        if (fence_error != ESP_OK) {
            ESP_LOGW(TAG, "Discarded OTA command from a stale MQTT session");
            break;
        }
        ota_manager_start_result_t claim_result =
            ota_manager_start(command.value.ota_update.url);
        mqtt_manager_command_commit_end();

        // Safety/NVS work, status publishing, and task creation may block, so
        // only the singleton claim above belongs under the session fence.
        ota_manager_start_result_t start_result =
            ota_manager_complete_start(claim_result);
        if (start_result != OTA_MANAGER_START_ACCEPTED) {
            ESP_LOGW(TAG, "OTA command rejected or could not be started (%d)",
                     (int)start_result);
        }
        break;
    }
    case COMMAND_ROUTER_COMMAND_NONE:
    default:
        break;
    }
}



static bool network_config_valid(void)
{
    bool valid = true;
    size_t ssid_len = strlen(CONFIG_WIFI_SSID);
    size_t password_len = strlen(CONFIG_WIFI_PASS);
    if (ssid_len == 0 || ssid_len > sizeof(((wifi_config_t *)0)->sta.ssid) ||
        password_len > sizeof(((wifi_config_t *)0)->sta.password)) {
        ESP_LOGE(TAG, "Wi-Fi credentials are empty or exceed driver limits");
        valid = false;
    }
    if (CONFIG_MQTT_BROKER_URI[0] == '\0' ||
        (strncmp(CONFIG_MQTT_BROKER_URI, "mqtt://", 7) != 0 &&
         strncmp(CONFIG_MQTT_BROKER_URI, "mqtts://", 8) != 0)) {
        ESP_LOGE(TAG, "MQTT broker URI must begin with mqtt:// or mqtts://");
        valid = false;
    }
#ifdef CONFIG_WIFI_REQUIRE_SECURE_AUTH
    if (password_len == 0) {
        ESP_LOGE(TAG, "Production Wi-Fi policy rejects open access points");
        valid = false;
    }
#endif
#ifdef CONFIG_MQTT_REQUIRE_TLS_AUTH
    if (strncmp(CONFIG_MQTT_BROKER_URI, "mqtts://", 8) != 0 ||
        CONFIG_MQTT_USERNAME[0] == '\0' || CONFIG_MQTT_PASSWORD[0] == '\0') {
        ESP_LOGE(TAG, "Production MQTT policy requires mqtts:// plus username/password");
        valid = false;
    }
#endif
#ifdef CONFIG_OTA_ENABLED
    if (!ota_manager_url_allowed(CONFIG_OTA_ALLOWED_URL_PREFIX)) {
        ESP_LOGE(TAG, "OTA URL prefix is invalid; OTA requests will be rejected");
    }
#endif
    return valid;
}


static bool running_image_pending_verify(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    return running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

static bool motors_hold_safe_zero(void)
{
    if (!motor_control_holds_safe_zero()) return false;
    safety_supervisor_health_t safety = safety_supervisor_health_snapshot();
    return !safety.hardware_interlock_present || !safety.interlock_enabled;
}


static bool pending_image_local_health_window_passed(void)
{
    int64_t started_us = esp_timer_get_time();
    uint32_t window_ms = 1250;
#ifdef CONFIG_SUPERVISOR_ENABLED
    if (window_ms < (uint32_t)CONFIG_SUPERVISOR_TASK_WDT_MS + 1000U) {
        window_ms = (uint32_t)CONFIG_SUPERVISOR_TASK_WDT_MS + 1000U;
    }
#endif
#ifdef CONFIG_SCHEDULE_ENABLED
    if (window_ms < 2750U) window_ms = 2750U;
#endif
#ifdef CONFIG_TACH_FEEDBACK_ENABLED
    if (window_ms < (uint32_t)CONFIG_TACH_SAMPLE_MS * 2U + 750U) {
        window_ms = (uint32_t)CONFIG_TACH_SAMPLE_MS * 2U + 750U;
    }
#endif
    int64_t deadline_us = started_us + (int64_t)window_ms * 1000;
    int64_t midpoint_us = started_us + (int64_t)window_ms * 500;
    bool midpoint_taken = false;
    int64_t ramp_mid = 0;
    int64_t supervisor_mid = 0;
    mqtt_manager_health_t mqtt_dispatch_mid = {0};
#ifdef CONFIG_SCHEDULE_ENABLED
    int64_t schedule_mid = 0;
#endif
#ifdef CONFIG_TACH_FEEDBACK_ENABLED
    int64_t tach_mid = 0;
    safety_supervisor_tach_action_health_t tach_action_mid = {0};
#endif

    while (esp_timer_get_time() < deadline_us) {
        if (!motors_hold_safe_zero()) {
            ESP_LOGE(TAG,
                     "Pending image did not maintain a zero-throttle RMT stream");
            return false;
        }
        if (!midpoint_taken && esp_timer_get_time() >= midpoint_us) {
            ramp_mid = motor_control_ramp_heartbeat_us();
            supervisor_mid = safety_supervisor_heartbeat_us();
            mqtt_dispatch_mid = mqtt_manager_health_snapshot();
#ifdef CONFIG_SCHEDULE_ENABLED
            schedule_mid = scheduler_health_snapshot().heartbeat_us;
#endif
#ifdef CONFIG_TACH_FEEDBACK_ENABLED
            tach_mid = tach_monitor_get_heartbeat_us();
            (void)read_tach_action_health(NULL, &tach_action_mid);
#endif
            midpoint_taken = true;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    int64_t ended_us = esp_timer_get_time();
    int64_t ramp_end = motor_control_ramp_heartbeat_us();
    int64_t supervisor_end = safety_supervisor_heartbeat_us();
    mqtt_manager_health_t mqtt_dispatch_end =
        mqtt_manager_health_snapshot();
#ifdef CONFIG_SCHEDULE_ENABLED
    scheduler_health_snapshot_t scheduler_health =
        scheduler_health_snapshot();
    int64_t schedule_end = scheduler_health.heartbeat_us;
#endif
#ifdef CONFIG_TACH_FEEDBACK_ENABLED
    int64_t tach_end = tach_monitor_get_heartbeat_us();
    safety_supervisor_tach_action_health_t tach_action_end = {0};
    (void)read_tach_action_health(NULL, &tach_action_end);
#endif
    bool auxiliary_tasks_ready = state_publisher_is_ready() &&
        mqtt_dispatch_end.command_dispatch_ready;
#ifdef CONFIG_RESTORE_FAN_STATE
    auxiliary_tasks_ready = auxiliary_tasks_ready &&
        fan_state_store_health_snapshot().save_task_ready;
#endif
#ifdef CONFIG_SCHEDULE_ENABLED
    auxiliary_tasks_ready = auxiliary_tasks_ready &&
                            scheduler_health.scheduler_task_ready &&
                            scheduler_health.save_task_ready;
#endif
#ifdef CONFIG_TACH_FEEDBACK_ENABLED
    auxiliary_tasks_ready = auxiliary_tasks_ready &&
                            tach_action_end.ready;
#endif

    bool tasks_healthy = midpoint_taken && auxiliary_tasks_ready &&
                         ramp_mid > started_us && ramp_end > ramp_mid &&
                         supervisor_mid > started_us &&
                         supervisor_end > supervisor_mid &&
                         mqtt_dispatch_mid.command_dispatch_ready &&
                         mqtt_dispatch_mid.command_dispatch_heartbeat_us >
                             started_us &&
                         mqtt_dispatch_end.command_dispatch_heartbeat_us >
                             mqtt_dispatch_mid.command_dispatch_heartbeat_us &&
                         ended_us -
                                 mqtt_dispatch_end.command_dispatch_heartbeat_us <
                             (int64_t)MQTT_DISPATCH_HEALTH_MAX_AGE_MS * 1000 &&
                         ended_us - ramp_end <
                             (int64_t)(s_app_config->ramp_tick_ms * 3 + 100) *
                                 1000 &&
                         ended_us - supervisor_end < 750LL * 1000;
#ifdef CONFIG_SCHEDULE_ENABLED
    tasks_healthy = tasks_healthy && schedule_mid > started_us &&
                    schedule_end > schedule_mid &&
                    ended_us - schedule_end < 1500LL * 1000;
#endif
#ifdef CONFIG_TACH_FEEDBACK_ENABLED
    tasks_healthy = tasks_healthy && tach_mid > started_us &&
                    tach_end > tach_mid &&
                    ended_us - tach_end <
                        (int64_t)(CONFIG_TACH_SAMPLE_MS + 500) * 1000 &&
                    tach_action_mid.ready &&
                    tach_action_mid.heartbeat_us > started_us &&
                    tach_action_end.heartbeat_us >
                        tach_action_mid.heartbeat_us &&
                    ended_us - tach_action_end.heartbeat_us <
                        (int64_t)(TACH_ACTION_HEARTBEAT_MS + 500) * 1000;
#endif
    if (!tasks_healthy) {
        ESP_LOGE(TAG, "Pending image critical-task health window failed");
        return false;
    }
    return motors_hold_safe_zero();
}


static void startup_failure_safe(const char *reason)
{
    bool pending_image = running_image_pending_verify();
    safety_supervisor_set_boot_health(SAFETY_BOOT_FAILED_SAFE);
    bool stopped = safety_stop_all(reason, pending_image, true);
    if (pending_image) {
        // A rollback is a deliberate reboot into an image that may restore fan
        // targets. Require both the fail-safe stop and an explicit acknowledged
        // zero-target commit, including when the supervisor callbacks were not
        // configured yet because the pending image failed early in boot.
        esp_err_t persistence_error = stopped
            ? fan_state_store_save_sync(
                  pdMS_TO_TICKS(ROLLBACK_PERSIST_TIMEOUT_MS))
            : ESP_ERR_INVALID_STATE;
        if (!stopped || persistence_error != ESP_OK) {
            ESP_LOGE(
                TAG,
                "OTA rollback suppressed: fail-safe stop or zero-state commit failed%s%s",
                persistence_error != ESP_OK ? ": " : "",
                persistence_error != ESP_OK
                    ? esp_err_to_name(persistence_error)
                    : "");
            safety_supervisor_set_boot_health(
                SAFETY_BOOT_ROLLBACK_FAILED_SAFE);
            for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
        }

        ESP_LOGE(TAG,
                 "Pending OTA image failed local self-test; rolling back after zero-state commit");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
        ESP_LOGE(TAG, "OTA rollback request failed: %s", esp_err_to_name(err));
        // Returning means the bootloader could not select a valid fallback.
        // Restarting the same pending image would weaken the failed-safe state,
        // so remain inhibited for service/recovery instead.
        safety_supervisor_set_boot_health(
            SAFETY_BOOT_ROLLBACK_FAILED_SAFE);
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static bool confirm_local_boot_health(void)
{
    if (!running_image_pending_verify()) {
        safety_supervisor_set_boot_health(SAFETY_BOOT_HEALTHY);
        return true;
    }
    if (!pending_image_local_health_window_passed()) {
        startup_failure_safe("OTA local health window failed");
        return false;
    }
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        safety_supervisor_set_boot_health(SAFETY_BOOT_HEALTHY);
        ESP_LOGI(TAG, "Pending OTA image passed local motor/task self-test");
        return true;
    }
    ESP_LOGE(TAG, "Could not validate pending OTA image: %s", esp_err_to_name(err));
    startup_failure_safe("OTA validation lifecycle failed");
    return false;
}

void app_main(void)
{
    // This must remain the first operation: assert the independent hardware
    // inhibit before parsing configuration or initializing any driver.
    esp_err_t ret = safety_supervisor_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Motor interlock initialization failed: %s",
                 esp_err_to_name(ret));
        startup_failure_safe("motor interlock initialization failed");
        return;
    }
    const esp_reset_reason_t reset_reason = esp_reset_reason();
    const bool restore_motion_state =
        reset_reason_allows_motion_restore(reset_reason);

    ESP_LOGI(TAG, "Validate node and motor configuration");
    ret = app_config_init();
    if (ret != ESP_OK) {
        startup_failure_safe("node or DShot configuration invalid");
        return;
    }
    s_app_config = app_config_get();
    if (!s_app_config) {
        startup_failure_safe("configuration ownership initialization failed");
        return;
    }
    s_command_router_config = (command_router_config_t) {
        .node_base_prefix = s_app_config->node_topic,
        .legacy_base_prefix = s_app_config->mqtt_root_topic,
        .fan_index_start = s_app_config->fan_index_start,
        .fan_count = s_app_config->fan_count,
#ifdef CONFIG_SCHEDULE_ENABLED
        .schedule_slot_count = CONFIG_SCHEDULE_SLOTS,
        .schedule_enabled = true,
#endif
#ifdef CONFIG_TACH_FEEDBACK_ENABLED
        .tach_enabled = true,
#endif
#ifdef CONFIG_OTA_ENABLED
        .ota_enabled = true,
        .ota_allowed_url_prefix = CONFIG_OTA_ALLOWED_URL_PREFIX,
#endif
#ifdef CONFIG_MQTT_LEGACY_TOPICS
        .legacy_enabled = true,
#endif
    };

    ret = mqtt_manager_init(mqtt_on_message, NULL);
    if (ret != ESP_OK) {
        startup_failure_safe("MQTT manager initialization failed");
        return;
    }
#ifdef CONFIG_OTA_ENABLED
    const ota_manager_config_t ota_config = {
        .prepare = ota_prepare_callback,
        .trusted_time = ota_trusted_time_callback,
        .abort = ota_abort_callback,
        .publish_status = ota_publish_status_callback,
    };
    ret = ota_manager_init(&ota_config);
    if (ret != ESP_OK) {
        startup_failure_safe("OTA manager initialization failed");
        return;
    }
#endif

    const safety_supervisor_callbacks_t safety_callbacks = {
        .inhibit_automatic = safety_inhibit_automatic_callback,
        .request_persistence = safety_request_persistence_callback,
        .notify_state = safety_notify_state_callback,
#ifdef CONFIG_TACH_FEEDBACK_ENABLED
        .read_tach_action_health = read_tach_action_health,
#endif
    };
    ret = safety_supervisor_configure(&safety_callbacks);
    if (ret != ESP_OK) {
        startup_failure_safe("safety supervisor configuration failed");
        return;
    }

    ESP_LOGI(TAG, "Init NVS");
    bool nvs_erased_on_boot = false;
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS cannot be initialized (%s); erasing and recreating it",
                 esp_err_to_name(ret));
        esp_err_t erase_err = nvs_flash_erase();
        if (erase_err == ESP_OK) {
            nvs_erased_on_boot = true;
            ret = nvs_flash_init();
        } else {
            ret = erase_err;
        }
    }
    if (ret != ESP_OK) {
        startup_failure_safe("NVS initialization failed");
        return;
    }

    const fan_state_store_config_t store_config = {
        .app_config = s_app_config,
        .snapshot_targets = fan_state_snapshot_targets,
        .restore_targets = fan_state_restore_targets,
    };
    ret = fan_state_store_init(&store_config, nvs_erased_on_boot);
    if (ret != ESP_OK) {
        // Target persistence remains usable even if the auxiliary erase-health
        // observation could not be committed.
        ESP_LOGE(TAG, "Could not persist NVS erase observation: %s",
                 esp_err_to_name(ret));
    }

    ret = safety_supervisor_configure_watchdog();
    if (ret != ESP_OK) {
        startup_failure_safe("task watchdog initialization failed");
        return;
    }

    ESP_LOGI(TAG, "Init DShot motors (%d) before networking",
             s_app_config->fan_count);
    ret = motor_control_init(s_app_config);
    if (ret != ESP_OK) {
        startup_failure_safe("DShot/RMT initialization failed");
        return;
    }
    // Establish a repeating protocol-defined zero-throttle stream before any
    // restored target, schedule, Wi-Fi, or MQTT operation.
    vTaskDelay(pdMS_TO_TICKS(MOTOR_BOOT_SETTLE_MS));

    const scheduler_config_t scheduler_config = {
        .app_config = s_app_config,
        .apply_target = scheduler_apply_target_callback,
        .notify = scheduler_notify_callback,
    };
    ret = scheduler_init(&scheduler_config);
    if (ret != ESP_OK) {
        startup_failure_safe("scheduler initialization failed");
        return;
    }
#ifdef CONFIG_TACH_FEEDBACK_ENABLED
    s_tach_action_mutex = xSemaphoreCreateMutex();
    if (!s_tach_action_mutex) {
        startup_failure_safe("tachometer action mutex allocation failed");
        return;
    }
    const tach_monitor_config_t tach_config = {
        .app_config = s_app_config,
        .read_motor_snapshot = tach_read_motor_snapshot,
        .confirmed_stall = tach_confirmed_stall,
    };
    ret = tach_monitor_init(&tach_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Tachometer initialization failed: %s",
                 esp_err_to_name(ret));
        startup_failure_safe("tachometer initialization failed");
        return;
    }
    if (xTaskCreate(tach_action_task, "tach_action",
                    TACH_ACTION_TASK_STACK, NULL,
                    TACH_ACTION_TASK_PRIORITY,
                    &s_tach_action_task) != pdPASS) {
        startup_failure_safe("tachometer action task creation failed");
        return;
    }
#endif

    const state_publisher_config_t publisher_config = {
        .app_config = s_app_config,
        .read_coordinator_health = read_publisher_coordinator_health,
    };
    ret = state_publisher_init(&publisher_config);
    if (ret != ESP_OK) {
        startup_failure_safe("state publisher initialization failed");
        return;
    }

    ret = fan_state_store_start();
    if (ret != ESP_OK) {
        startup_failure_safe("fan-state persistence task creation failed");
        return;
    }
    ret = state_publisher_start();
    if (ret != ESP_OK) {
        startup_failure_safe("state publisher task creation failed");
        return;
    }
    ret = motor_control_start_ramp(motor_event_callback, NULL);
    if (ret != ESP_OK) {
        startup_failure_safe("motor ramp task creation failed");
        return;
    }
    ret = scheduler_start();
    if (ret != ESP_OK) {
        startup_failure_safe("scheduler task creation failed");
        return;
    }

    // Close recovery state before any later task initialization can delay the
    // boot path. Persisted schedules are uptime-relative (minimum one minute),
    // and this inhibit is committed immediately after their task is created.
    if (!restore_motion_state) {
        ESP_LOGW(TAG,
                 "Reset reason %d is not trusted for motion restore; committing zero targets and a schedule inhibit",
                 (int)reset_reason);
        esp_err_t schedule_error = scheduler_try_inhibit_all(
            pdMS_TO_TICKS(ROLLBACK_PERSIST_TIMEOUT_MS));
        esp_err_t target_error = fan_state_store_save_sync(
            pdMS_TO_TICKS(ROLLBACK_PERSIST_TIMEOUT_MS));
        if (schedule_error != ESP_OK || target_error != ESP_OK) {
            startup_failure_safe(
                "unsafe-reset motion-state invalidation failed");
            return;
        }
    }
#ifdef CONFIG_TACH_FEEDBACK_ENABLED
    ret = tach_monitor_start();
    if (ret != ESP_OK) {
        startup_failure_safe("tachometer task creation failed");
        return;
    }
#endif
    ret = safety_supervisor_start();
    if (ret != ESP_OK) {
        startup_failure_safe("supervisor task creation failed");
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(
        (uint32_t)s_app_config->ramp_tick_ms * 3U));
    if (motor_control_ramp_heartbeat_us() == 0) {
        startup_failure_safe("motor ramp local self-test failed");
        return;
    }
    if (!confirm_local_boot_health()) return;
#if defined(CONFIG_COMM_LOSS_STOP_AFTER_LEASE) || \
    defined(CONFIG_MQTT_RESTART_ON_TIMEOUT)
    mqtt_manager_begin_ack_window();
#endif
#ifdef CONFIG_RESTORE_FAN_STATE
    if (restore_motion_state) fan_state_store_restore();
#endif
    if (!interlock_enable_if_safe()) {
        startup_failure_safe(
            "motor interlock safety policy rejected startup enable");
        return;
    }


    if (!network_config_valid()) {
        ESP_LOGE(TAG, "Network services disabled; local motor control remains active");
        safety_supervisor_set_boot_health(
            SAFETY_BOOT_HEALTHY_OFFLINE_CONFIG);
        return;
    }

    ESP_LOGI(TAG, "Init Wi‑Fi");
    ret = wifi_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi initialization failed: %s; local control remains active",
                 esp_err_to_name(ret));
        safety_supervisor_set_boot_health(
            SAFETY_BOOT_HEALTHY_OFFLINE_WIFI);
        return;
    }

    // Wait for an IP, but don't hang forever — if the AP is unreachable,
    // restart and retry so the device recovers unattended.
    bool has_ip = wifi_manager_wait_for_ip(pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    if (!has_ip) {
        ESP_LOGW(TAG, "Wi-Fi startup grace expired; entering degraded local mode");
    }

    bool tls_time_needed = strncmp(CONFIG_MQTT_BROKER_URI, "mqtts://", 8) == 0;
#ifdef CONFIG_OTA_ENABLED
    tls_time_needed = true;
#endif
    if (has_ip && tls_time_needed && !wifi_manager_system_time_valid_for_tls()) {
        esp_err_t time_err = wifi_manager_wait_for_time(
            pdMS_TO_TICKS(CONFIG_TLS_TIME_SYNC_TIMEOUT_MS));
        if (time_err != ESP_OK || !wifi_manager_system_time_valid_for_tls()) {
            ESP_LOGE(TAG, "TLS time sync not ready; secure network services remain offline");
        }
    }

    ESP_LOGI(TAG, "Init MQTT");
    if (has_ip &&
        (strncmp(CONFIG_MQTT_BROKER_URI, "mqtts://", 8) != 0 ||
         wifi_manager_system_time_valid_for_tls())) {
        esp_err_t mqtt_err = mqtt_manager_start();
        if (mqtt_err != ESP_OK) {
            ESP_LOGE(TAG, "MQTT startup deferred: %s", esp_err_to_name(mqtt_err));
        }
    }

    ESP_LOGI(TAG, "Setup complete");

    while (1) {
        mqtt_manager_poll();
        if (!mqtt_manager_connected() && wifi_manager_has_ip() &&
            (strncmp(CONFIG_MQTT_BROKER_URI, "mqtts://", 8) != 0 ||
             wifi_manager_system_time_valid_for_tls())) {
            esp_err_t mqtt_err = mqtt_manager_start();
            if (mqtt_err != ESP_OK) {
                ESP_LOGW(TAG, "MQTT startup still deferred: %s", esp_err_to_name(mqtt_err));
            }
        }
        if (mqtt_manager_ready()) {
            if (mqtt_manager_take_initial_publish()) {
                state_publisher_publish_discovery_and_metadata();
            }
            state_publisher_request_all();
            state_publisher_publish_periodic_metrics_health();
        }
#ifdef CONFIG_MQTT_RESTART_ON_TIMEOUT
        {
            int64_t now = esp_timer_get_time();
            mqtt_manager_health_t mqtt_health = mqtt_manager_health_snapshot();
            int64_t last_ack = mqtt_health.last_ack_us;
#ifdef CONFIG_OTA_ENABLED
            if (!ota_manager_is_in_progress())
#endif
            if (last_ack > 0 &&
                (now - last_ack) > (CONFIG_MQTT_WATCHDOG_TIMEOUT_MS * 1000ULL)) {
                ESP_LOGE(TAG, "MQTT watchdog timeout (%d ms); restarting",
                         (int)((now - last_ack) / 1000));
                if (safety_stop_all("MQTT watchdog timeout", true, true)) {
                    // Motors are durably inhibited and the cleared manual
                    // targets have been synchronously committed.
                    vTaskDelay(pdMS_TO_TICKS(100));
                    esp_restart();
                }
                ESP_LOGE(TAG,
                         "MQTT restart suppressed: fail-safe stop or zero-state commit failed");
            }
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS));
    }
}

