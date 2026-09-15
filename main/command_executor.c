#include "command_executor.h"
#include <string.h>
#include "command_router.h"
#include "esp_log.h"
#include "fan_state_store.h"
#include "motor_control.h"
#include "ota_manager.h"
#include "safety_supervisor.h"
#include "scheduler.h"
#include "state_publisher.h"
#include "tach_actions.h"

#define MQTT_COMMAND_COMMIT_WAIT_MS 1500
static const char *TAG = "command_executor";
static const app_config_t *s_app_config;
static command_router_config_t s_command_router_config;

esp_err_t command_executor_init(const app_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    if (s_app_config) return ESP_ERR_INVALID_STATE;
    s_app_config = config;
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
    return ESP_OK;
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

void command_executor_on_message(void *context, const char *topic, const char *data,
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
                (void)safety_supervisor_enable_output_if_safe();
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
        state_publisher_request_fan(command.fan_local_index, true);
        break;
    }
    case COMMAND_ROUTER_COMMAND_TACH_CLEAR: {
        // The tach worker may legitimately hold its serialization mutex for a
        // multi-second safety stop. Wait for it before taking the short MQTT
        // generation fence, then revalidate immediately at the clear commit.
        esp_err_t err = tach_actions_clear_begin();
        if (err == ESP_OK) {
            err = mqtt_manager_command_commit_begin(
                session, pdMS_TO_TICKS(MQTT_COMMAND_COMMIT_WAIT_MS));
            if (err == ESP_OK) {
                err = tach_actions_clear_locked(command.fan_local_index);
                mqtt_manager_command_commit_end();
            }
            tach_actions_clear_end();
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "fan%d tach alarm clear failed", command.fan_index);
            return;
        }
        state_publisher_request_fan(command.fan_local_index, true);
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



