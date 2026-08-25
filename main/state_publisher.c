#include "state_publisher.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "fan_state_store.h"
#include "freertos/task.h"
#include "motor_control.h"
#include "mqtt_manager.h"
#include "tach_monitor.h"
#include "wifi_manager.h"

#define STATE_PUBLISHER_TASK_STACK    4096
#define STATE_PUBLISHER_TASK_PRIORITY 5
#define STATE_PUBLISHER_FULL_BIT      (UINT32_C(1) << 31)
#define STATE_PUBLISHER_FAN_MASK      ((UINT32_C(1) << APP_CONFIG_MAX_FANS) - 1U)

static const char *TAG = "state_publisher";
static portMUX_TYPE s_lifecycle_lock = portMUX_INITIALIZER_UNLOCKED;
static state_publisher_config_t s_config;
static TaskHandle_t s_task;
static bool s_init_started;
static bool s_initialized;
static bool s_start_started;
static bool s_task_ready;
static uint32_t s_pending_before_start;

static bool local_fan_valid(int local_fan)
{
    return s_config.app_config && local_fan >= 0 &&
           local_fan < s_config.app_config->fan_count;
}

static void get_coordinator_health(
    state_publisher_coordinator_health_t *out_snapshot)
{
    if (!out_snapshot) return;
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    snprintf(out_snapshot->boot_health, sizeof(out_snapshot->boot_health),
             "unknown");

    /* The callback is deliberately outside every publisher critical section. */
    if (s_config.read_coordinator_health) {
        s_config.read_coordinator_health(s_config.callback_context,
                                         out_snapshot);
    }
    out_snapshot->boot_health[sizeof(out_snapshot->boot_health) - 1] = '\0';
}

static bool publish_fan_leaf(int fan_number, const char *leaf,
                             const char *payload, int qos, bool retain,
                             bool mirror_legacy)
{
    char topic[MQTT_MANAGER_TOPIC_CAPACITY];
    bool published = false;
    if (app_config_format_node_topic(topic, sizeof(topic), "fan%d/%s",
                                     fan_number, leaf)) {
        published = mqtt_manager_publish(topic, payload, qos, retain);
    }
#ifdef CONFIG_MQTT_LEGACY_TOPICS
    if (mirror_legacy &&
        app_config_format_legacy_topic(topic, sizeof(topic), "fan%d/%s",
                                       fan_number, leaf)) {
        published = mqtt_manager_publish(topic, payload, qos, retain) && published;
    }
#else
    (void)mirror_legacy;
#endif
    return published;
}

static void publish_discovery(void)
{
    char availability[MQTT_MANAGER_TOPIC_CAPACITY];
    if (!app_config_format_node_topic(availability, sizeof(availability),
                                      "status")) {
        return;
    }

#ifdef CONFIG_HOME_ASSISTANT_DISCOVERY_ENABLED
    for (int i = 0; i < s_config.app_config->fan_count; ++i) {
        int fan_number = s_config.app_config->fan_index_start + i;
        char discovery[160];
        char unique_id[96];
        char command[MQTT_MANAGER_TOPIC_CAPACITY];
        char state[MQTT_MANAGER_TOPIC_CAPACITY];
        char pct_command[MQTT_MANAGER_TOPIC_CAPACITY];
        char pct_state[MQTT_MANAGER_TOPIC_CAPACITY];
        snprintf(discovery, sizeof(discovery),
                 "homeassistant/fan/%s_fan%d/config",
                 s_config.app_config->node_id, fan_number);
        snprintf(unique_id, sizeof(unique_id), "%s_fan%d",
                 s_config.app_config->node_id, fan_number);
        if (!app_config_format_node_topic(command, sizeof(command),
                                          "fan%d/set", fan_number) ||
            !app_config_format_node_topic(state, sizeof(state),
                                          "fan%d/state", fan_number) ||
            !app_config_format_node_topic(pct_command, sizeof(pct_command),
                                          "fan%d/percentage/set", fan_number) ||
            !app_config_format_node_topic(pct_state, sizeof(pct_state),
                                          "fan%d/percentage", fan_number)) {
            continue;
        }

        char payload[896];
        int written = snprintf(
            payload, sizeof(payload),
            "{\"name\":\"Tent Fan %d\",\"unique_id\":\"%s\","
            "\"command_topic\":\"%s\",\"state_topic\":\"%s\","
            "\"percentage_command_topic\":\"%s\","
            "\"percentage_state_topic\":\"%s\","
            "\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
            "\"availability_topic\":\"%s\","
            "\"payload_available\":\"online\","
            "\"payload_not_available\":\"offline\"}",
            fan_number, unique_id, command, state, pct_command, pct_state,
            availability);
        if (written > 0 && (size_t)written < sizeof(payload)) {
            (void)mqtt_manager_publish(discovery, payload, 1, true);
        }

#ifdef CONFIG_TACH_FEEDBACK_ENABLED
        char sensor_discovery[176];
        char rpm_topic[MQTT_MANAGER_TOPIC_CAPACITY];
        char sensor_payload[640];
        snprintf(sensor_discovery, sizeof(sensor_discovery),
                 "homeassistant/sensor/%s_fan%d_rpm/config",
                 s_config.app_config->node_id, fan_number);
        if (app_config_format_node_topic(rpm_topic, sizeof(rpm_topic),
                                         "fan%d/measured_rpm", fan_number)) {
            written = snprintf(
                sensor_payload, sizeof(sensor_payload),
                "{\"name\":\"Tent Fan %d RPM\","
                "\"unique_id\":\"%s_fan%d_rpm\","
                "\"state_topic\":\"%s\","
                "\"unit_of_measurement\":\"rpm\","
                "\"availability_topic\":\"%s\"}",
                fan_number, s_config.app_config->node_id, fan_number,
                rpm_topic, availability);
            if (written > 0 && (size_t)written < sizeof(sensor_payload)) {
                (void)mqtt_manager_publish(sensor_discovery, sensor_payload, 1,
                                           true);
            }
        }
#endif
    }
#endif

    (void)mqtt_manager_publish(availability, "online", 1, true);
}

static void publish_node_metadata(void)
{
    char info_topic[MQTT_MANAGER_TOPIC_CAPACITY];
    char fans_topic[MQTT_MANAGER_TOPIC_CAPACITY];
    char announce_topic[MQTT_MANAGER_TOPIC_CAPACITY];
    if (!app_config_format_node_topic(info_topic, sizeof(info_topic), "info") ||
        !app_config_format_node_topic(fans_topic, sizeof(fans_topic), "fans") ||
        !app_config_format_node_topic(announce_topic, sizeof(announce_topic),
                                      "announce")) {
        return;
    }

    state_publisher_coordinator_health_t coordinator;
    get_coordinator_health(&coordinator);
    fan_state_store_health_t store = fan_state_store_health_snapshot();
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    char node_info[512];
    int info_len = snprintf(
        node_info, sizeof(node_info),
        "{\"node\":\"%s\",\"fan_start\":%d,\"fan_count\":%d,"
        "\"project\":\"%s\",\"app_version\":\"%s\","
        "\"idf_version\":\"%s\",\"partition\":\"%s\","
        "\"reset_reason\":%d,\"boot_health\":\"%s\","
        "\"nvs_erased_on_boot\":%s,\"nvs_erase_observed\":%s,"
        "\"schedules\":%d,\"ota\":%d,\"legacy_topics\":%d,"
        "\"home_assistant_discovery\":%d}",
        s_config.app_config->node_id,
        s_config.app_config->fan_index_start,
        s_config.app_config->fan_count,
        app ? app->project_name : "unknown",
        app ? app->version : "unknown",
        app ? app->idf_ver : IDF_VER,
        (running && running->label[0]) ? running->label : "unknown",
        (int)esp_reset_reason(), coordinator.boot_health,
        store.erased_on_boot ? "true" : "false",
        store.erase_observed ? "true" : "false",
#ifdef CONFIG_SCHEDULE_ENABLED
        CONFIG_SCHEDULE_SLOTS,
#else
        0,
#endif
#ifdef CONFIG_OTA_ENABLED
        1,
#else
        0,
#endif
#ifdef CONFIG_MQTT_LEGACY_TOPICS
        1,
#else
        0,
#endif
#ifdef CONFIG_HOME_ASSISTANT_DISCOVERY_ENABLED
        1
#else
        0
#endif
    );
    if (info_len > 0 && (size_t)info_len < sizeof(node_info)) {
        (void)mqtt_manager_publish(info_topic, node_info, 1, true);
    } else {
        ESP_LOGE(TAG, "Node metadata exceeded its bounded payload buffer");
    }

    char fans[48];
    int position = snprintf(fans, sizeof(fans), "[");
    for (int i = 0;
         i < s_config.app_config->fan_count && position > 0 &&
         position < (int)sizeof(fans);
         ++i) {
        position += snprintf(fans + position, sizeof(fans) - (size_t)position,
                             "%s%d", i ? "," : "",
                             s_config.app_config->fan_index_start + i);
    }
    if (position > 0 && position < (int)sizeof(fans) - 1) {
        (void)snprintf(fans + position, sizeof(fans) - (size_t)position, "]");
        (void)mqtt_manager_publish(fans_topic, fans, 1, true);
    }

    char announce[144];
    int announce_len = snprintf(
        announce, sizeof(announce),
        "{\"event\":\"node_up\",\"node\":\"%s\",\"fans\":%d}",
        s_config.app_config->node_id, s_config.app_config->fan_count);
    if (announce_len > 0 && (size_t)announce_len < sizeof(announce)) {
        (void)mqtt_manager_publish(announce_topic, announce, 1, false);
    }
}

static void publish_one_state(int local_fan, int qos, bool retain)
{
    if (!mqtt_manager_ready() || !local_fan_valid(local_fan)) return;

    motor_control_fan_snapshot_t motor;
    if (!motor_control_get_fan_snapshot(local_fan, &motor)) return;

    int fan_number = s_config.app_config->fan_index_start + local_fan;
    char value[24];
    (void)publish_fan_leaf(fan_number, "state",
                           motor.applied_pct ? "ON" : "OFF", qos, retain,
                           true);
    (void)snprintf(value, sizeof(value), "%u", motor.applied_pct);
    (void)publish_fan_leaf(fan_number, "percentage", value, qos, retain, true);
    (void)publish_fan_leaf(fan_number, "applied_percentage", value, qos,
                           retain, false);
    (void)snprintf(value, sizeof(value), "%u", motor.target_pct);
    (void)publish_fan_leaf(fan_number, "requested_percentage", value, qos,
                           retain, false);
    if (retain) {
        (void)snprintf(value, sizeof(value), "%" PRIu32,
                       motor.accepted_command_count);
        (void)publish_fan_leaf(fan_number, "accepted_command_count", value, 1,
                               true, false);
    }

#ifdef CONFIG_TACH_FEEDBACK_ENABLED
    tach_monitor_snapshot_t tach;
    if (tach_monitor_get_snapshot(local_fan, &tach) == ESP_OK) {
        if (tach.measurement_valid) {
            (void)snprintf(value, sizeof(value), "%" PRIu32,
                           tach.measured_rpm);
            (void)publish_fan_leaf(fan_number, "measured_rpm", value, qos,
                                   retain, false);
        }
        (void)publish_fan_leaf(fan_number, "alarm",
                               tach.stall_alarm ? "ON" : "OFF", qos, retain,
                               false);
        const char *alarm_reason = "none";
        if (tach.fault_cause == TACH_MONITOR_FAULT_STALL) {
            alarm_reason = "stall";
        } else if (tach.fault_cause ==
                   TACH_MONITOR_FAULT_SENSOR_INVALID) {
            alarm_reason = "sensor_invalid";
        }
        (void)publish_fan_leaf(fan_number, "alarm_reason", alarm_reason,
                               qos, retain, false);
    }
#endif
}

static void state_publisher_task(void *argument)
{
    (void)argument;
    portENTER_CRITICAL(&s_lifecycle_lock);
    s_task_ready = true;
    portEXIT_CRITICAL(&s_lifecycle_lock);

    while (true) {
        uint32_t bits = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        bool full_retained = (bits & STATE_PUBLISHER_FULL_BIT) != 0;
        uint32_t fans = bits & STATE_PUBLISHER_FAN_MASK;
        for (int i = 0; i < s_config.app_config->fan_count; ++i) {
            if ((fans & (UINT32_C(1) << i)) != 0) {
                publish_one_state(i, full_retained ? 1 : 0, full_retained);
            }
        }
    }
}

static void publish_metrics(void)
{
    if (!mqtt_manager_ready()) return;
    char value[32];
    for (int i = 0; i < s_config.app_config->fan_count; ++i) {
        motor_control_fan_snapshot_t motor;
        if (!motor_control_get_fan_snapshot(i, &motor)) continue;
        int fan_number = s_config.app_config->fan_index_start + i;
        (void)snprintf(value, sizeof(value), "%" PRIu32,
                       motor.rmt_refresh_count);
        (void)publish_fan_leaf(fan_number, "rmt_refresh_count", value, 1,
                               true, true);
        (void)snprintf(value, sizeof(value), "%" PRIu32,
                       motor.rmt_error_total);
        (void)publish_fan_leaf(fan_number, "rmt_error_total", value, 1, true,
                               false);
        (void)snprintf(value, sizeof(value), "%u",
                       motor.rmt_error_consecutive);
        (void)publish_fan_leaf(fan_number, "rmt_error_consecutive", value, 1,
                               true, false);
    }
}

static void publish_health_probe(void)
{
    if (!mqtt_manager_ready()) return;

    wifi_manager_snapshot_t wifi;
    wifi_manager_get_snapshot(&wifi);
    mqtt_manager_health_t mqtt = mqtt_manager_health_snapshot();
    fan_state_store_health_t store = fan_state_store_health_snapshot();
    state_publisher_coordinator_health_t coordinator;
    get_coordinator_health(&coordinator);
    int64_t now = esp_timer_get_time();
    char topic[MQTT_MANAGER_TOPIC_CAPACITY];
    if (!app_config_format_node_topic(topic, sizeof(topic), "health")) return;

    int64_t mqtt_dispatch_age_ms =
        mqtt.oldest_command_since_us > 0 &&
                now > mqtt.oldest_command_since_us
            ? (now - mqtt.oldest_command_since_us) / 1000
            : 0;
    char payload[1152];
    int written = snprintf(
        payload, sizeof(payload),
        "{\"uptime_ms\":%" PRIi64 ",\"free_heap\":%" PRIu32 ","
        "\"minimum_free_heap\":%" PRIu32 ",\"mqtt_ready\":%s,"
        "\"mqtt_ack_age_ms\":%" PRIi64 ",\"mqtt_pubacks\":%" PRIu32 ","
        "\"mqtt_subacks\":%" PRIu32 ",\"mqtt_connects\":%" PRIu32 ","
        "\"mqtt_publish_failures\":%" PRIu32 ",\"mqtt_outbox_bytes\":%d,"
        "\"mqtt_command_queue_drops\":%" PRIu32 ","
        "\"mqtt_pending_commands\":%u,"
        "\"mqtt_dispatch_ready\":%s,\"mqtt_command_in_flight\":%s,"
        "\"mqtt_dispatch_age_ms\":%" PRIi64 ","
        "\"mqtt_dispatch_stack_words\":%" PRIu32 ","
        "\"wifi_degraded\":%s,\"wifi_disconnects\":%" PRIu32 ","
        "\"nvs_erased_on_boot\":%s,\"nvs_erase_observed\":%s,"
        "\"interlock_enabled\":%s,\"comm_failsafe\":%s,"
        "\"global_safety_latched\":%s,\"ramp_stack_words\":%u,"
        "\"supervisor_stack_words\":%u,\"ramp_liveness_fault\":%s,"
        "\"scheduler_liveness_fault\":%s,\"tach_liveness_fault\":%s,"
        "\"tach_action_ready\":%s,\"tach_action_pending\":%s,"
        "\"tach_action_age_ms\":%" PRIi64 ","
        "\"tach_action_stack_words\":%" PRIu32 ","
        "\"mqtt_command_overflow_fault\":%s,"
        "\"mqtt_dispatch_liveness_fault\":%s}",
        now / 1000, esp_get_free_heap_size(), esp_get_minimum_free_heap_size(),
        mqtt.ready ? "true" : "false",
        mqtt.last_ack_us > 0 ? (now - mqtt.last_ack_us) / 1000 : -1,
        mqtt.puback_count, mqtt.suback_count, mqtt.connect_count,
        mqtt.publish_failures, mqtt.outbox_bytes,
        mqtt.command_queue_drops, (unsigned)mqtt.pending_command_count,
        mqtt.command_dispatch_ready ? "true" : "false",
        mqtt.command_in_flight ? "true" : "false",
        mqtt_dispatch_age_ms, mqtt.command_dispatch_stack_words,
        wifi.degraded ? "true" : "false", wifi.disconnect_count,
        store.erased_on_boot ? "true" : "false",
        store.erase_observed ? "true" : "false",
        coordinator.interlock_enabled ? "true" : "false",
        coordinator.communication_failsafe_active ? "true" : "false",
        coordinator.global_safety_latched ? "true" : "false",
        (unsigned)motor_control_ramp_stack_words(),
        (unsigned)coordinator.supervisor_stack_words,
        coordinator.ramp_liveness_fault ? "true" : "false",
        coordinator.scheduler_liveness_fault ? "true" : "false",
        coordinator.tach_liveness_fault ? "true" : "false",
        coordinator.tach_action_ready ? "true" : "false",
        coordinator.tach_action_pending ? "true" : "false",
        coordinator.tach_action_age_ms,
        coordinator.tach_action_stack_words,
        coordinator.mqtt_command_overflow_fault ? "true" : "false",
        coordinator.mqtt_dispatch_liveness_fault ? "true" : "false");
    if (written > 0 && (size_t)written < sizeof(payload)) {
        /* QoS-1 PUBACK is the recurring broker round-trip proof for the lease. */
        (void)mqtt_manager_publish(topic, payload, 1, false);
    } else {
        ESP_LOGE(TAG, "Health payload exceeded its bounded buffer");
    }
}

esp_err_t state_publisher_init(const state_publisher_config_t *config)
{
    if (!config || !config->app_config ||
        !config->read_coordinator_health ||
        config->app_config != app_config_get() ||
        config->app_config->fan_count < 1 ||
        config->app_config->fan_count > APP_CONFIG_MAX_FANS) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lifecycle_lock);
    if (s_initialized) {
        bool same = s_config.app_config == config->app_config &&
                    s_config.read_coordinator_health ==
                        config->read_coordinator_health &&
                    s_config.callback_context == config->callback_context;
        portEXIT_CRITICAL(&s_lifecycle_lock);
        return same ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (s_init_started) {
        portEXIT_CRITICAL(&s_lifecycle_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_init_started = true;
    s_config = *config;
    s_initialized = true;
    portEXIT_CRITICAL(&s_lifecycle_lock);
    return ESP_OK;
}

esp_err_t state_publisher_start(void)
{
    portENTER_CRITICAL(&s_lifecycle_lock);
    if (!s_initialized) {
        portEXIT_CRITICAL(&s_lifecycle_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_task) {
        portEXIT_CRITICAL(&s_lifecycle_lock);
        return ESP_OK;
    }
    if (s_start_started) {
        portEXIT_CRITICAL(&s_lifecycle_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_start_started = true;
    portEXIT_CRITICAL(&s_lifecycle_lock);

    TaskHandle_t task = NULL;
    if (xTaskCreate(state_publisher_task, "state_pub",
                    STATE_PUBLISHER_TASK_STACK, NULL,
                    STATE_PUBLISHER_TASK_PRIORITY, &task) != pdPASS) {
        portENTER_CRITICAL(&s_lifecycle_lock);
        s_start_started = false;
        portEXIT_CRITICAL(&s_lifecycle_lock);
        ESP_LOGE(TAG, "Failed to create state publisher task");
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_lifecycle_lock);
    s_task = task;
    uint32_t pending = s_pending_before_start;
    s_pending_before_start = 0;
    portEXIT_CRITICAL(&s_lifecycle_lock);
    if (pending != 0) (void)xTaskNotify(task, pending, eSetBits);
    return ESP_OK;
}

void state_publisher_request_fan(int local_fan, bool full_retained)
{
    if (!local_fan_valid(local_fan)) return;
    uint32_t bits = UINT32_C(1) << local_fan;
    if (full_retained) bits |= STATE_PUBLISHER_FULL_BIT;

    portENTER_CRITICAL(&s_lifecycle_lock);
    TaskHandle_t task = s_task;
    if (!task) s_pending_before_start |= bits;
    portEXIT_CRITICAL(&s_lifecycle_lock);
    if (task) (void)xTaskNotify(task, bits, eSetBits);
}

void state_publisher_request_all(void)
{
    if (!s_config.app_config) return;
    uint32_t fan_bits =
        (UINT32_C(1) << s_config.app_config->fan_count) - UINT32_C(1);
    portENTER_CRITICAL(&s_lifecycle_lock);
    TaskHandle_t task = s_task;
    if (!task) {
        s_pending_before_start |= fan_bits | STATE_PUBLISHER_FULL_BIT;
    }
    portEXIT_CRITICAL(&s_lifecycle_lock);
    if (task) {
        (void)xTaskNotify(task, fan_bits | STATE_PUBLISHER_FULL_BIT, eSetBits);
    }
}

void state_publisher_publish_discovery_and_metadata(void)
{
    if (!s_initialized) return;
    publish_discovery();
    publish_node_metadata();
}

void state_publisher_publish_periodic_metrics_health(void)
{
    if (!s_initialized) return;
    publish_metrics();
    publish_health_probe();
}

bool state_publisher_publish_manual_ack(int local_fan,
                                        uint32_t accepted_command_count,
                                        uint8_t requested_pct,
                                        uint8_t applied_pct)
{
    if (!local_fan_valid(local_fan) || requested_pct > 100 || applied_pct > 100) {
        return false;
    }
    char acknowledgement[144];
    int written = snprintf(
        acknowledgement, sizeof(acknowledgement),
        "{\"count\":%" PRIu32 ",\"requested\":%u,\"applied\":%u,"
        "\"outcome\":\"accepted\"}",
        accepted_command_count, requested_pct, applied_pct);
    if (written <= 0 || (size_t)written >= sizeof(acknowledgement)) return false;
    int fan_number = s_config.app_config->fan_index_start + local_fan;
    return publish_fan_leaf(fan_number, "ack", acknowledgement, 0, false,
                            false);
}

bool state_publisher_publish_schedule_entry(
    int local_fan, unsigned int slot,
    const scheduler_entry_snapshot_t *entry)
{
    if (!local_fan_valid(local_fan) || !entry) return false;
#ifdef CONFIG_SCHEDULE_ENABLED
    if (slot >= CONFIG_SCHEDULE_SLOTS) return false;
#else
    (void)slot;
    return false;
#endif

#ifdef CONFIG_SCHEDULE_ENABLED
    char payload[128];
    int written = snprintf(payload, sizeof(payload), "%" PRIu32 ",%" PRIu32
                           ",%u", entry->interval_min, entry->duration_min,
                           entry->target_pct);
    if (written <= 0 || (size_t)written >= sizeof(payload)) return false;
    int fan_number = s_config.app_config->fan_index_start + local_fan;
    char topic[MQTT_MANAGER_TOPIC_CAPACITY];
    if (!app_config_format_node_topic(topic, sizeof(topic), "schedule/%d/%u",
                                      fan_number, slot)) {
        return false;
    }
    return mqtt_manager_publish(topic, payload, 1, true);
#endif
}

bool state_publisher_publish_ota_status(const char *status, bool retain)
{
#ifndef CONFIG_OTA_ENABLED
    (void)status;
    (void)retain;
    return false;
#else
    if (!s_initialized || !status || status[0] == '\0') return false;
    char topic[MQTT_MANAGER_TOPIC_CAPACITY];
    if (!app_config_format_node_topic(topic, sizeof(topic), "ota/status")) {
        return false;
    }
    return mqtt_manager_publish(topic, status, 1, retain);
#endif
}

bool state_publisher_is_ready(void)
{
    portENTER_CRITICAL(&s_lifecycle_lock);
    bool ready = s_task_ready;
    portEXIT_CRITICAL(&s_lifecycle_lock);
    return ready;
}

UBaseType_t state_publisher_stack_high_water_mark(void)
{
    portENTER_CRITICAL(&s_lifecycle_lock);
    TaskHandle_t task = s_task;
    portEXIT_CRITICAL(&s_lifecycle_lock);
    return task ? uxTaskGetStackHighWaterMark(task) : 0;
}
