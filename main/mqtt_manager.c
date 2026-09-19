#include "mqtt_manager.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"

#include "app_config.h"

#ifndef CONFIG_MQTT_BROKER_URI
#define CONFIG_MQTT_BROKER_URI ""
#endif
#ifndef CONFIG_MQTT_USERNAME
#define CONFIG_MQTT_USERNAME ""
#endif
#ifndef CONFIG_MQTT_PASSWORD
#define CONFIG_MQTT_PASSWORD ""
#endif
#ifndef CONFIG_MQTT_OUTBOX_LIMIT_BYTES
#define CONFIG_MQTT_OUTBOX_LIMIT_BYTES 16384
#endif
#ifndef CONFIG_MQTT_SUBSCRIBE_TIMEOUT_MS
#define CONFIG_MQTT_SUBSCRIBE_TIMEOUT_MS 10000
#endif

#define MQTT_MANAGER_MAX_PENDING_SUBSCRIPTIONS 32
#define MQTT_MANAGER_STOPPED_REBUILD_GRACE_US  250000
#define MQTT_MANAGER_COMMAND_QUEUE_DEPTH       8
#define MQTT_MANAGER_DISPATCH_TASK_STACK        8192
#define MQTT_MANAGER_DISPATCH_TASK_PRIORITY     5
#define MQTT_MANAGER_DISPATCH_HEARTBEAT_MS       250

typedef struct {
    char topic[MQTT_MANAGER_TOPIC_CAPACITY];
    char payload[MQTT_MANAGER_PAYLOAD_CAPACITY];
    int total_len;
    int next_offset;
    uint32_t connection_generation;
    bool retained;
    bool dropping;
} mqtt_rx_assembly_t;

typedef struct {
    char topic[MQTT_MANAGER_TOPIC_CAPACITY];
    char payload[MQTT_MANAGER_PAYLOAD_CAPACITY];
    size_t payload_length;
    uint32_t connection_generation;
    uint32_t dispatch_sequence;
    int64_t queued_at_us;
    uint8_t pending_slot;
    bool retained;
} mqtt_dispatch_message_t;

typedef struct {
    bool occupied;
    uint32_t sequence;
    int64_t queued_at_us;
} mqtt_dispatch_pending_record_t;

static const char *TAG = "mqtt_manager";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized;
static bool s_initializing;
static bool s_starting;
static esp_mqtt_client_handle_t s_client;
static mqtt_manager_message_callback_t s_message_callback;
static void *s_callback_context;
static QueueHandle_t s_dispatch_queue;
static TaskHandle_t s_dispatch_task;
// Serializes application-task ESP-MQTT API calls with client replacement.
// Event handling and application command callbacks never hold this mutex.
static SemaphoreHandle_t s_lifecycle_mutex;
// Linearizes application command commits with connection-generation changes.
// Command callbacks must release this before any MQTT publish operation.
static SemaphoreHandle_t s_command_commit_mutex;

static bool s_connected;
static bool s_ready;
static bool s_initial_publish_pending;
static bool s_disconnect_requested;
static bool s_subscription_setup_failed;
static int64_t s_last_ack_us;
static int64_t s_last_rx_us;
static int64_t s_connected_since_us;
static uint32_t s_puback_count;
static uint32_t s_publish_failures;
static uint32_t s_suback_count;
static uint32_t s_connect_count;
static uint32_t s_connection_generation;
static uint32_t s_command_dispatch_count;
static bool s_command_dispatch_ready;
static int64_t s_command_dispatch_heartbeat_us;
static int64_t s_command_in_flight_since_us;
static uint32_t s_next_dispatch_sequence;
static mqtt_dispatch_pending_record_t
    s_dispatch_pending[MQTT_MANAGER_COMMAND_QUEUE_DEPTH];
static uint32_t s_command_queue_drops;
static uint32_t s_stale_command_drops;
static uint32_t s_dispatch_allocation_failures;
static int64_t s_stopped_state_since_us;
static int s_pending_sub_ids[MQTT_MANAGER_MAX_PENDING_SUBSCRIPTIONS];
static size_t s_pending_sub_count;

static mqtt_rx_assembly_t s_rx;
// DATA events are serialized on ESP-MQTT's event task. This scratch object
// avoids putting a roughly 1 KiB queue item on that task's stack.
static mqtt_dispatch_message_t s_dispatch_scratch;
// ESP-MQTT retains configuration strings, so these buffers must outlive start().
static char s_client_id[64];
static char s_availability_topic[MQTT_MANAGER_TOPIC_CAPACITY];

static SemaphoreHandle_t mqtt_manager_lock_command_commits(void)
{
    portENTER_CRITICAL(&s_lock);
    SemaphoreHandle_t mutex = s_command_commit_mutex;
    portEXIT_CRITICAL(&s_lock);
    if (mutex && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) return mutex;
    return NULL;
}

static void mqtt_manager_unlock_command_commits(SemaphoreHandle_t mutex)
{
    if (mutex) xSemaphoreGive(mutex);
}

static bool mqtt_manager_system_time_valid_for_tls(void)
{
    // TLS performs the authoritative certificate notBefore/notAfter check. This
    // only rejects the reset-time epoch and other implausibly old wall clocks.
    time_t now = time(NULL);
    return now >= (time_t)1704067200; // 2024-01-01T00:00:00Z
}

static void mqtt_manager_rx_reset(void)
{
    memset(&s_rx, 0, sizeof(s_rx));
}

static void mqtt_manager_mark_subscription_failure(void)
{
    portENTER_CRITICAL(&s_lock);
    s_subscription_setup_failed = true;
    s_disconnect_requested = true;
    portEXIT_CRITICAL(&s_lock);
}

static void mqtt_manager_dispatch_task(void *arg)
{
    QueueHandle_t queue = (QueueHandle_t)arg;
    mqtt_dispatch_message_t message;

    portENTER_CRITICAL(&s_lock);
    s_command_dispatch_ready = true;
    s_command_dispatch_heartbeat_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_lock);

    for (;;) {
        if (xQueueReceive(
                queue, &message,
                pdMS_TO_TICKS(MQTT_MANAGER_DISPATCH_HEARTBEAT_MS)) != pdTRUE) {
            portENTER_CRITICAL(&s_lock);
            s_command_dispatch_heartbeat_us = esp_timer_get_time();
            portEXIT_CRITICAL(&s_lock);
            continue;
        }

        portENTER_CRITICAL(&s_lock);
        if (message.pending_slot < MQTT_MANAGER_COMMAND_QUEUE_DEPTH) {
            mqtt_dispatch_pending_record_t *record =
                &s_dispatch_pending[message.pending_slot];
            if (record->occupied &&
                record->sequence == message.dispatch_sequence) {
                *record = (mqtt_dispatch_pending_record_t) {0};
            }
        }
        s_command_in_flight_since_us = message.queued_at_us;
        bool current = s_connected && s_ready &&
                       message.connection_generation == s_connection_generation;
        mqtt_manager_message_callback_t callback = s_message_callback;
        void *context = s_callback_context;
        if (!current || !callback) s_stale_command_drops++;
        portEXIT_CRITICAL(&s_lock);

        if (!current || !callback) {
            portENTER_CRITICAL(&s_lock);
            s_command_in_flight_since_us = 0;
            s_command_dispatch_heartbeat_us = esp_timer_get_time();
            portEXIT_CRITICAL(&s_lock);
            continue;
        }
        mqtt_manager_command_session_t session = {
            .connection_generation = message.connection_generation,
        };
        callback(context, message.topic, message.payload,
                 message.payload_length, message.retained, &session);

        portENTER_CRITICAL(&s_lock);
        s_command_dispatch_count++;
        s_command_in_flight_since_us = 0;
        s_command_dispatch_heartbeat_us = esp_timer_get_time();
        portEXIT_CRITICAL(&s_lock);
    }
}

static void mqtt_manager_enqueue_assembled_message(
    esp_mqtt_client_handle_t source_client, uint32_t source_generation)
{
    SemaphoreHandle_t commit_mutex = mqtt_manager_lock_command_commits();
    QueueHandle_t queue;
    portENTER_CRITICAL(&s_lock);
    bool current = source_client == s_client && s_connected && s_ready &&
                   source_generation == s_connection_generation;
    queue = s_dispatch_queue;
    int pending_slot = -1;
    uint32_t dispatch_sequence = 0;
    int64_t queued_at_us = esp_timer_get_time();
    if (current && queue) {
        for (int i = 0; i < MQTT_MANAGER_COMMAND_QUEUE_DEPTH; ++i) {
            if (!s_dispatch_pending[i].occupied) {
                pending_slot = i;
                break;
            }
        }
        if (pending_slot >= 0) {
            dispatch_sequence = ++s_next_dispatch_sequence;
            if (dispatch_sequence == 0) {
                dispatch_sequence = ++s_next_dispatch_sequence;
            }
            s_dispatch_pending[pending_slot] =
                (mqtt_dispatch_pending_record_t) {
                    .occupied = true,
                    .sequence = dispatch_sequence,
                    .queued_at_us = queued_at_us,
                };
        }
    }
    if (!current) s_stale_command_drops++;
    portEXIT_CRITICAL(&s_lock);
    if (!current) {
        mqtt_manager_unlock_command_commits(commit_mutex);
        return;
    }

    size_t topic_length = strlen(s_rx.topic);
    memcpy(s_dispatch_scratch.topic, s_rx.topic, topic_length + 1);
    memcpy(s_dispatch_scratch.payload, s_rx.payload,
           (size_t)s_rx.total_len + 1);
    s_dispatch_scratch.payload_length = (size_t)s_rx.total_len;
    s_dispatch_scratch.connection_generation = s_rx.connection_generation;
    s_dispatch_scratch.dispatch_sequence = dispatch_sequence;
    s_dispatch_scratch.queued_at_us = queued_at_us;
    s_dispatch_scratch.pending_slot =
        pending_slot >= 0 ? (uint8_t)pending_slot : UINT8_MAX;
    s_dispatch_scratch.retained = s_rx.retained;

    if (!queue || pending_slot < 0 ||
        xQueueSend(queue, &s_dispatch_scratch, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_lock);
        if (pending_slot >= 0) {
            mqtt_dispatch_pending_record_t *record =
                &s_dispatch_pending[pending_slot];
            if (record->occupied && record->sequence == dispatch_sequence) {
                *record = (mqtt_dispatch_pending_record_t) {0};
            }
        }
        s_command_queue_drops++;
        // Missing a queue item would violate ordering. Invalidate every
        // command from this session and reconnect instead of accepting a
        // partially observed command stream.
        s_ready = false;
        s_disconnect_requested = true;
        s_connection_generation++;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGE(TAG,
                 "MQTT command queue saturated; disconnect requested fail-closed");
    }
    mqtt_manager_unlock_command_commits(commit_mutex);
}

static bool mqtt_manager_subscribe_required(esp_mqtt_client_handle_t client,
                                            const char *topic)
{
    if (!client || !topic || !topic[0]) {
        mqtt_manager_mark_subscription_failure();
        return false;
    }

    portENTER_CRITICAL(&s_lock);
    bool capacity_available =
        s_pending_sub_count < MQTT_MANAGER_MAX_PENDING_SUBSCRIPTIONS;
    portEXIT_CRITICAL(&s_lock);
    if (!capacity_available) {
        ESP_LOGE(TAG, "Required MQTT subscription table is full");
        mqtt_manager_mark_subscription_failure();
        return false;
    }

    int message_id = esp_mqtt_client_subscribe_single(client, topic, 1);
    if (message_id < 0) {
        ESP_LOGE(TAG, "Required MQTT subscription failed for '%s'", topic);
        mqtt_manager_mark_subscription_failure();
        return false;
    }

    portENTER_CRITICAL(&s_lock);
    if (s_pending_sub_count < MQTT_MANAGER_MAX_PENDING_SUBSCRIPTIONS) {
        s_pending_sub_ids[s_pending_sub_count++] = message_id;
        capacity_available = true;
    } else {
        s_subscription_setup_failed = true;
        s_disconnect_requested = true;
        capacity_available = false;
    }
    portEXIT_CRITICAL(&s_lock);
    return capacity_available;
}

static bool mqtt_manager_subscribe_node(esp_mqtt_client_handle_t client,
                                        const char *suffix_format, int fan_index)
{
    char topic[MQTT_MANAGER_TOPIC_CAPACITY];
    if (!app_config_format_node_topic(topic, sizeof(topic),
                                      suffix_format, fan_index)) {
        ESP_LOGE(TAG, "Could not format required node-scoped MQTT topic");
        mqtt_manager_mark_subscription_failure();
        return false;
    }
    return mqtt_manager_subscribe_required(client, topic);
}

#ifdef CONFIG_MQTT_LEGACY_TOPICS
static bool mqtt_manager_subscribe_legacy(esp_mqtt_client_handle_t client,
                                          const char *suffix_format, int fan_index)
{
    char topic[MQTT_MANAGER_TOPIC_CAPACITY];
    if (!app_config_format_legacy_topic(topic, sizeof(topic),
                                        suffix_format, fan_index)) {
        ESP_LOGE(TAG, "Could not format required legacy MQTT topic");
        mqtt_manager_mark_subscription_failure();
        return false;
    }
    return mqtt_manager_subscribe_required(client, topic);
}
#endif

#if defined(CONFIG_OTA_ENABLED) || defined(CONFIG_SCHEDULE_ENABLED)
static bool mqtt_manager_subscribe_node_literal(esp_mqtt_client_handle_t client,
                                                const char *suffix)
{
    char topic[MQTT_MANAGER_TOPIC_CAPACITY];
    if (!app_config_format_node_topic(topic, sizeof(topic), "%s", suffix)) {
        ESP_LOGE(TAG, "Could not format required node-scoped MQTT topic");
        mqtt_manager_mark_subscription_failure();
        return false;
    }
    return mqtt_manager_subscribe_required(client, topic);
}
#endif

#if defined(CONFIG_MQTT_LEGACY_TOPICS) && defined(CONFIG_SCHEDULE_ENABLED)
static bool mqtt_manager_subscribe_legacy_literal(esp_mqtt_client_handle_t client,
                                                  const char *suffix)
{
    char topic[MQTT_MANAGER_TOPIC_CAPACITY];
    if (!app_config_format_legacy_topic(topic, sizeof(topic), "%s", suffix)) {
        ESP_LOGE(TAG, "Could not format required legacy MQTT topic");
        mqtt_manager_mark_subscription_failure();
        return false;
    }
    return mqtt_manager_subscribe_required(client, topic);
}
#endif

static void mqtt_manager_subscribe_all(esp_mqtt_client_handle_t client)
{
    const app_config_t *config = app_config_get();
    portENTER_CRITICAL(&s_lock);
    s_pending_sub_count = 0;
    s_subscription_setup_failed = false;
    portEXIT_CRITICAL(&s_lock);
    if (!config) {
        mqtt_manager_mark_subscription_failure();
        return;
    }

    for (int i = 0; i < config->fan_count; ++i) {
        int fan_index = config->fan_index_start + i;
        (void)mqtt_manager_subscribe_node(client, "fan%d/set", fan_index);
        (void)mqtt_manager_subscribe_node(client, "fan%d/percentage/set", fan_index);
#ifdef CONFIG_TACH_FEEDBACK_ENABLED
        (void)mqtt_manager_subscribe_node(client, "fan%d/alarm/clear", fan_index);
#endif
#ifdef CONFIG_MQTT_LEGACY_TOPICS
        (void)mqtt_manager_subscribe_legacy(client, "fan%d/set", fan_index);
        (void)mqtt_manager_subscribe_legacy(client, "fan%d/percentage/set", fan_index);
#endif
    }
#ifdef CONFIG_OTA_ENABLED
    (void)mqtt_manager_subscribe_node_literal(client, "ota/update");
#endif
#ifdef CONFIG_SCHEDULE_ENABLED
    static const char *const suffixes[] = {
        "schedule/+/+/set",
        "schedule/+/+/disable",
        "schedule/+/+/get",
        "schedule/override",
    };
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
        (void)mqtt_manager_subscribe_node_literal(client, suffixes[i]);
#ifdef CONFIG_MQTT_LEGACY_TOPICS
        (void)mqtt_manager_subscribe_legacy_literal(client, suffixes[i]);
#endif
    }
#endif
}

static bool mqtt_manager_remove_pending_subscription_locked(int message_id)
{
    for (size_t i = 0; i < s_pending_sub_count; ++i) {
        if (s_pending_sub_ids[i] != message_id) continue;
        s_pending_sub_ids[i] = s_pending_sub_ids[s_pending_sub_count - 1];
        s_pending_sub_count--;
        return true;
    }
    return false;
}

static void mqtt_manager_handle_data_event(esp_mqtt_event_handle_t event)
{
    if (!event) return;

    portENTER_CRITICAL(&s_lock);
    bool current_client = event->client == s_client && s_connected && s_ready;
    uint32_t current_generation = s_connection_generation;
    if (current_client) s_last_rx_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_lock);
    if (!current_client) return;

    if (event->total_data_len < 0 ||
        event->current_data_offset < 0 || event->data_len < 0) {
        ESP_LOGW(TAG, "Rejected MQTT message with invalid fragment lengths");
        mqtt_manager_rx_reset();
        return;
    }

    const int total = event->total_data_len;
    const int offset = event->current_data_offset;
    const int chunk = event->data_len;

    if (offset == 0) {
        mqtt_manager_rx_reset();
        bool invalid_topic = !event->topic || event->topic_len <= 0 ||
                             event->topic_len >= MQTT_MANAGER_TOPIC_CAPACITY ||
                             memchr(event->topic, '\0', event->topic_len) != NULL;
        bool invalid_payload = total >= MQTT_MANAGER_PAYLOAD_CAPACITY ||
                               chunk > total || (chunk > 0 && !event->data);
        if (invalid_topic || invalid_payload) {
            ESP_LOGW(TAG, "Rejected oversized or malformed MQTT message");
            if (chunk <= total) {
                s_rx.dropping = true;
                s_rx.total_len = total;
                s_rx.next_offset = chunk;
                if (chunk == total) mqtt_manager_rx_reset();
            }
            return;
        }

        memcpy(s_rx.topic, event->topic, event->topic_len);
        s_rx.topic[event->topic_len] = '\0';
        s_rx.total_len = total;
        s_rx.connection_generation = current_generation;
        s_rx.retained = event->retain;
    }

    if (s_rx.dropping) {
        if (total != s_rx.total_len || offset != s_rx.next_offset ||
            offset > total || chunk > total - offset) {
            mqtt_manager_rx_reset();
            return;
        }
        s_rx.next_offset += chunk;
        if (s_rx.next_offset == total) mqtt_manager_rx_reset();
        return;
    }

    if (s_rx.topic[0] == '\0' ||
        s_rx.connection_generation != current_generation ||
        total != s_rx.total_len ||
        offset != s_rx.next_offset || offset > total || chunk > total - offset ||
        (chunk > 0 && !event->data)) {
        ESP_LOGW(TAG, "Rejected out-of-order MQTT fragment");
        mqtt_manager_rx_reset();
        return;
    }

    if (chunk > 0) memcpy(s_rx.payload + offset, event->data, chunk);
    s_rx.next_offset += chunk;
    if (s_rx.next_offset == total) {
        s_rx.payload[total] = '\0';
        mqtt_manager_enqueue_assembled_message(event->client,
                                               current_generation);
        mqtt_manager_rx_reset();
    }
}

static void mqtt_manager_event_handler(void *handler_args, esp_event_base_t base,
                                       int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    if (!event) return;

    switch (event_id) {
    case MQTT_EVENT_CONNECTED: {
        int64_t now = esp_timer_get_time();
        SemaphoreHandle_t commit_mutex = mqtt_manager_lock_command_commits();
        portENTER_CRITICAL(&s_lock);
        bool current_client = event->client == s_client;
        if (!current_client) {
            portEXIT_CRITICAL(&s_lock);
            mqtt_manager_unlock_command_commits(commit_mutex);
            break;
        }
        s_connected = true;
        s_ready = false;
        s_initial_publish_pending = false;
        s_disconnect_requested = false;
        s_connected_since_us = now;
        s_connect_count++;
        s_connection_generation++;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "MQTT transport connected; waiting for required SUBACKs");
        // The commit fence prevents client replacement until every required
        // subscribe call and its pending-ID bookkeeping has completed.
        mqtt_manager_subscribe_all(event->client);
        portENTER_CRITICAL(&s_lock);
        current_client = event->client == s_client;
        bool setup_failed = current_client && s_subscription_setup_failed;
        bool no_pending = current_client && s_pending_sub_count == 0;
        if (current_client && (setup_failed || no_pending)) {
            s_disconnect_requested = true;
        }
        portEXIT_CRITICAL(&s_lock);
        mqtt_manager_unlock_command_commits(commit_mutex);
        if (setup_failed || no_pending) {
            ESP_LOGE(TAG, "MQTT required subscription setup failed");
        }
        break;
    }
    case MQTT_EVENT_SUBSCRIBED: {
        bool accepted = event->data && event->data_len > 0;
        for (int i = 0; accepted && i < event->data_len; ++i) {
            if ((uint8_t)event->data[i] >= 0x80) accepted = false;
        }
        if (event->error_handle &&
            event->error_handle->error_type == MQTT_ERROR_TYPE_SUBSCRIBE_FAILED) {
            accepted = false;
        }
        bool current_client;
        bool found = false;
        bool became_ready = false;
        portENTER_CRITICAL(&s_lock);
        current_client = event->client == s_client;
        if (current_client) {
            found = mqtt_manager_remove_pending_subscription_locked(
                event->msg_id);
        }
        if (found && accepted) {
            s_suback_count++;
            s_last_ack_us = esp_timer_get_time();
            if (s_pending_sub_count == 0 && !s_subscription_setup_failed) {
                s_ready = true;
                s_initial_publish_pending = true;
                became_ready = true;
            }
        } else if (found) {
            s_subscription_setup_failed = true;
            s_disconnect_requested = true;
        }
        portEXIT_CRITICAL(&s_lock);
        if (!current_client) break;
        if (!found) {
            ESP_LOGW(TAG, "Ignoring unexpected MQTT SUBACK id=%d", event->msg_id);
            break;
        }
        if (!accepted) {
            ESP_LOGE(TAG, "Broker rejected required MQTT subscription id=%d",
                     event->msg_id);
            break;
        }
        if (became_ready) ESP_LOGI(TAG, "MQTT command plane ready");
        break;
    }
    case MQTT_EVENT_PUBLISHED: {
        portENTER_CRITICAL(&s_lock);
        bool current_client = event->client == s_client;
        if (current_client) {
            s_puback_count++;
            s_last_ack_us = esp_timer_get_time();
        }
        portEXIT_CRITICAL(&s_lock);
        break;
    }
    case MQTT_EVENT_DISCONNECTED: {
        SemaphoreHandle_t commit_mutex = mqtt_manager_lock_command_commits();
        portENTER_CRITICAL(&s_lock);
        bool current_client = event->client == s_client;
        if (!current_client) {
            portEXIT_CRITICAL(&s_lock);
            mqtt_manager_unlock_command_commits(commit_mutex);
            break;
        }
        s_connected = false;
        s_ready = false;
        s_initial_publish_pending = false;
        s_disconnect_requested = false;
        s_pending_sub_count = 0;
        s_connection_generation++;
        portEXIT_CRITICAL(&s_lock);
        mqtt_manager_unlock_command_commits(commit_mutex);
        mqtt_manager_rx_reset();
        break;
    }
    case MQTT_EVENT_DATA:
        mqtt_manager_handle_data_event(event);
        break;
    case MQTT_EVENT_ERROR: {
        portENTER_CRITICAL(&s_lock);
        bool current_client = event->client == s_client;
        portEXIT_CRITICAL(&s_lock);
        if (current_client) {
            ESP_LOGE(TAG,
                     "MQTT error: link/down, authentication, or subscription failure");
        }
        break;
    }
    default:
        break;
    }
}

esp_err_t mqtt_manager_init(mqtt_manager_message_callback_t message_callback,
                            void *callback_context)
{
    if (!message_callback) return ESP_ERR_INVALID_ARG;
    if (!app_config_get()) return ESP_ERR_INVALID_STATE;

    portENTER_CRITICAL(&s_lock);
    if (s_initialized || s_initializing) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_initializing = true;
    portEXIT_CRITICAL(&s_lock);

    SemaphoreHandle_t lifecycle_mutex = xSemaphoreCreateMutex();
    if (!lifecycle_mutex) {
        portENTER_CRITICAL(&s_lock);
        s_dispatch_allocation_failures++;
        s_initializing = false;
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }

    SemaphoreHandle_t command_commit_mutex = xSemaphoreCreateMutex();
    if (!command_commit_mutex) {
        vSemaphoreDelete(lifecycle_mutex);
        portENTER_CRITICAL(&s_lock);
        s_dispatch_allocation_failures++;
        s_initializing = false;
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }

    QueueHandle_t queue = xQueueCreate(MQTT_MANAGER_COMMAND_QUEUE_DEPTH,
                                       sizeof(mqtt_dispatch_message_t));
    if (!queue) {
        vSemaphoreDelete(command_commit_mutex);
        vSemaphoreDelete(lifecycle_mutex);
        portENTER_CRITICAL(&s_lock);
        s_dispatch_allocation_failures++;
        s_initializing = false;
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }

    TaskHandle_t task = NULL;
    if (xTaskCreate(mqtt_manager_dispatch_task, "mqtt_cmd",
                    MQTT_MANAGER_DISPATCH_TASK_STACK, queue,
                    MQTT_MANAGER_DISPATCH_TASK_PRIORITY, &task) != pdPASS) {
        vQueueDelete(queue);
        vSemaphoreDelete(command_commit_mutex);
        vSemaphoreDelete(lifecycle_mutex);
        portENTER_CRITICAL(&s_lock);
        s_dispatch_allocation_failures++;
        s_initializing = false;
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_lock);
    s_message_callback = message_callback;
    s_callback_context = callback_context;
    s_dispatch_queue = queue;
    s_dispatch_task = task;
    s_lifecycle_mutex = lifecycle_mutex;
    s_command_commit_mutex = command_commit_mutex;
    s_initialized = true;
    s_initializing = false;
    portEXIT_CRITICAL(&s_lock);
    mqtt_manager_rx_reset();
    return ESP_OK;
}

esp_err_t mqtt_manager_command_commit_begin(
    const mqtt_manager_command_session_t *session, TickType_t timeout_ticks)
{
    if (!session) return ESP_ERR_INVALID_ARG;

    portENTER_CRITICAL(&s_lock);
    SemaphoreHandle_t mutex = s_command_commit_mutex;
    bool initialized = s_initialized;
    portEXIT_CRITICAL(&s_lock);
    if (!initialized || !mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(mutex, timeout_ticks) != pdTRUE) return ESP_ERR_TIMEOUT;

    portENTER_CRITICAL(&s_lock);
    bool current = s_connected && s_ready &&
                   session->connection_generation == s_connection_generation;
    if (!current) s_stale_command_drops++;
    portEXIT_CRITICAL(&s_lock);
    if (!current) {
        xSemaphoreGive(mutex);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

void mqtt_manager_command_commit_end(void)
{
    portENTER_CRITICAL(&s_lock);
    SemaphoreHandle_t mutex = s_command_commit_mutex;
    portEXIT_CRITICAL(&s_lock);
    if (mutex) xSemaphoreGive(mutex);
}

static esp_err_t mqtt_manager_start_locked(void)
{
    const app_config_t *config = app_config_get();
    portENTER_CRITICAL(&s_lock);
    bool initialized = s_initialized;
    esp_mqtt_client_handle_t existing_client = s_client;
    portEXIT_CRITICAL(&s_lock);
    if (!initialized || !config) return ESP_ERR_INVALID_STATE;
    if (strncmp(CONFIG_MQTT_BROKER_URI, "mqtts://", 8) == 0 &&
        !mqtt_manager_system_time_valid_for_tls()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (existing_client) {
        esp_mqtt_client_connection_state_t state =
            esp_mqtt_client_get_state(existing_client);
        if (state == MQTT_CLIENT_STATE_CONNECTED ||
            state == MQTT_CLIENT_STATE_WAITING_RECONNECT ||
            state == MQTT_CLIENT_STATE_NOT_STARTED) {
            portENTER_CRITICAL(&s_lock);
            if (s_client == existing_client) s_stopped_state_since_us = 0;
            portEXIT_CRITICAL(&s_lock);
            return ESP_OK;
        }

        // ESP-MQTT reports DISCONNECTED after its task exits on an
        // unrecoverable startup failure. Give the task time to finish its
        // epilogue, then replace the entire client so partial transport state
        // cannot strand all future start attempts.
        int64_t now = esp_timer_get_time();
        portENTER_CRITICAL(&s_lock);
        if (s_client != existing_client || s_starting) {
            portEXIT_CRITICAL(&s_lock);
            return ESP_ERR_INVALID_STATE;
        }
        if (s_stopped_state_since_us == 0) {
            s_stopped_state_since_us = now;
            portEXIT_CRITICAL(&s_lock);
            return ESP_ERR_INVALID_STATE;
        }
        if (now - s_stopped_state_since_us <
            MQTT_MANAGER_STOPPED_REBUILD_GRACE_US) {
            portEXIT_CRITICAL(&s_lock);
            return ESP_ERR_INVALID_STATE;
        }
        portEXIT_CRITICAL(&s_lock);

        SemaphoreHandle_t commit_mutex = mqtt_manager_lock_command_commits();
        portENTER_CRITICAL(&s_lock);
        if (s_client != existing_client || s_starting || s_connected) {
            portEXIT_CRITICAL(&s_lock);
            mqtt_manager_unlock_command_commits(commit_mutex);
            return ESP_ERR_INVALID_STATE;
        }
        s_starting = true;
        s_client = NULL;
        s_connected = false;
        s_ready = false;
        s_initial_publish_pending = false;
        s_pending_sub_count = 0;
        s_stopped_state_since_us = 0;
        s_connection_generation++;
        portEXIT_CRITICAL(&s_lock);
        mqtt_manager_unlock_command_commits(commit_mutex);
        (void)esp_mqtt_client_destroy(existing_client);
    } else {
        portENTER_CRITICAL(&s_lock);
        if (s_client || s_starting) {
            portEXIT_CRITICAL(&s_lock);
            return ESP_ERR_INVALID_STATE;
        }
        s_starting = true;
        portEXIT_CRITICAL(&s_lock);
    }

    int client_length = snprintf(s_client_id, sizeof(s_client_id),
                                 "dshot-%s", config->node_id);
    if (client_length <= 0 || (size_t)client_length >= sizeof(s_client_id) ||
        !app_config_format_node_topic(s_availability_topic,
                                      sizeof(s_availability_topic), "status")) {
        portENTER_CRITICAL(&s_lock);
        s_starting = false;
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_mqtt_client_config_t client_config = {
        .broker.address.uri = CONFIG_MQTT_BROKER_URI,
        .credentials.username =
            CONFIG_MQTT_USERNAME[0] ? CONFIG_MQTT_USERNAME : NULL,
        .credentials.authentication.password =
            CONFIG_MQTT_PASSWORD[0] ? CONFIG_MQTT_PASSWORD : NULL,
        .credentials.client_id = s_client_id,
        .session.keepalive = 30,
        .network.reconnect_timeout_ms = 3000,
        .network.timeout_ms = 5000,
        .outbox.limit = CONFIG_MQTT_OUTBOX_LIMIT_BYTES,
        .session.last_will = {
            .topic = s_availability_topic,
            .msg = "offline",
            .msg_len = 7,
            .qos = 1,
            .retain = true,
        },
    };
    if (strncmp(CONFIG_MQTT_BROKER_URI, "mqtts://", 8) == 0) {
        client_config.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    } else {
        ESP_LOGW(TAG,
                 "MQTT transport is not encrypted; prefer an mqtts:// broker URI");
    }

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&client_config);
    if (!client) {
        portENTER_CRITICAL(&s_lock);
        s_starting = false;
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_lock);
    s_client = client;
    s_stopped_state_since_us = 0;
    portEXIT_CRITICAL(&s_lock);
    esp_err_t err = esp_mqtt_client_register_event(
        client, ESP_EVENT_ANY_ID, mqtt_manager_event_handler, NULL);
    if (err == ESP_OK) err = esp_mqtt_client_start(client);
    if (err != ESP_OK) {
        SemaphoreHandle_t commit_mutex = mqtt_manager_lock_command_commits();
        portENTER_CRITICAL(&s_lock);
        if (s_client == client) s_client = NULL;
        s_connected = false;
        s_ready = false;
        s_initial_publish_pending = false;
        s_pending_sub_count = 0;
        s_connection_generation++;
        portEXIT_CRITICAL(&s_lock);
        mqtt_manager_unlock_command_commits(commit_mutex);
        (void)esp_mqtt_client_destroy(client);
    }
    portENTER_CRITICAL(&s_lock);
    s_starting = false;
    portEXIT_CRITICAL(&s_lock);
    return err;
}

esp_err_t mqtt_manager_start(void)
{
    portENTER_CRITICAL(&s_lock);
    SemaphoreHandle_t lifecycle_mutex = s_lifecycle_mutex;
    bool initialized = s_initialized;
    portEXIT_CRITICAL(&s_lock);
    if (!initialized || !lifecycle_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(lifecycle_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = mqtt_manager_start_locked();
    xSemaphoreGive(lifecycle_mutex);
    return err;
}

bool mqtt_manager_publish(const char *topic, const char *payload,
                          int qos, bool retain)
{
    if (!topic || !payload) return false;
    portENTER_CRITICAL(&s_lock);
    SemaphoreHandle_t lifecycle_mutex = s_lifecycle_mutex;
    portEXIT_CRITICAL(&s_lock);
    if (!lifecycle_mutex ||
        xSemaphoreTake(lifecycle_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    portENTER_CRITICAL(&s_lock);
    esp_mqtt_client_handle_t client = s_client;
    bool connected = s_connected;
    portEXIT_CRITICAL(&s_lock);
    if (!client || !connected) {
        xSemaphoreGive(lifecycle_mutex);
        return false;
    }

    int message_id = esp_mqtt_client_publish(client, topic, payload, 0, qos, retain);
    if (message_id >= 0) {
        xSemaphoreGive(lifecycle_mutex);
        return true;
    }

    portENTER_CRITICAL(&s_lock);
    s_publish_failures++;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGW(TAG, "MQTT publish dropped (result=%d, outbox=%d)",
             message_id, esp_mqtt_client_get_outbox_size(client));
    xSemaphoreGive(lifecycle_mutex);
    return false;
}

void mqtt_manager_poll(void)
{
    portENTER_CRITICAL(&s_lock);
    SemaphoreHandle_t lifecycle_mutex = s_lifecycle_mutex;
    portEXIT_CRITICAL(&s_lock);
    if (!lifecycle_mutex ||
        xSemaphoreTake(lifecycle_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    bool subscribe_timed_out =
        s_connected && !s_ready && s_connected_since_us > 0 &&
        now - s_connected_since_us >
            (int64_t)CONFIG_MQTT_SUBSCRIBE_TIMEOUT_MS * 1000;
    bool disconnect = s_disconnect_requested || subscribe_timed_out;
    if (disconnect) s_disconnect_requested = false;
    esp_mqtt_client_handle_t client = s_client;
    portEXIT_CRITICAL(&s_lock);
    if (!disconnect || !client) {
        xSemaphoreGive(lifecycle_mutex);
        return;
    }

    // This function is intentionally called outside the MQTT event task and
    // outside all motor/TWDT-critical tasks: these APIs may wait on networking.
    ESP_LOGW(TAG, "Disconnecting MQTT transport: command plane is not healthy");
    char status_topic[MQTT_MANAGER_TOPIC_CAPACITY];
    if (app_config_format_node_topic(status_topic, sizeof(status_topic), "status")) {
        (void)esp_mqtt_client_publish(client, status_topic, "offline", 0, 1, true);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    (void)esp_mqtt_client_disconnect(client);
    xSemaphoreGive(lifecycle_mutex);
}

bool mqtt_manager_connected(void)
{
    portENTER_CRITICAL(&s_lock);
    bool connected = s_connected;
    portEXIT_CRITICAL(&s_lock);
    return connected;
}

bool mqtt_manager_ready(void)
{
    portENTER_CRITICAL(&s_lock);
    bool ready = s_ready;
    portEXIT_CRITICAL(&s_lock);
    return ready;
}

bool mqtt_manager_take_initial_publish(void)
{
    portENTER_CRITICAL(&s_lock);
    bool pending = s_initial_publish_pending;
    s_initial_publish_pending = false;
    portEXIT_CRITICAL(&s_lock);
    return pending;
}

void mqtt_manager_begin_ack_window(void)
{
    portENTER_CRITICAL(&s_lock);
    s_last_ack_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_lock);
}

mqtt_manager_health_t mqtt_manager_health_snapshot(void)
{
    mqtt_manager_health_t snapshot;
    QueueHandle_t dispatch_queue;
    SemaphoreHandle_t lifecycle_mutex;
    bool client_exists;
    TaskHandle_t dispatch_task;
    portENTER_CRITICAL(&s_lock);
    snapshot = (mqtt_manager_health_t) {
        .connected = s_connected,
        .ready = s_ready,
        .last_ack_us = s_last_ack_us,
        .last_rx_us = s_last_rx_us,
        .connected_since_us = s_connected_since_us,
        .puback_count = s_puback_count,
        .publish_failures = s_publish_failures,
        .suback_count = s_suback_count,
        .connect_count = s_connect_count,
        .command_dispatch_count = s_command_dispatch_count,
        .command_dispatch_ready = s_command_dispatch_ready,
        .command_in_flight = s_command_in_flight_since_us > 0,
        .command_dispatch_heartbeat_us =
            s_command_dispatch_heartbeat_us,
        .oldest_command_since_us = s_command_in_flight_since_us,
        .command_dispatch_stack_bytes = 0,
        .command_queue_drops = s_command_queue_drops,
        .stale_command_drops = s_stale_command_drops,
        .dispatch_allocation_failures = s_dispatch_allocation_failures,
        .pending_subscription_count = s_pending_sub_count,
        .pending_command_count = 0,
        .outbox_bytes = 0,
    };
    client_exists = s_client != NULL;
    for (int i = 0; i < MQTT_MANAGER_COMMAND_QUEUE_DEPTH; ++i) {
        int64_t queued_at_us = s_dispatch_pending[i].occupied
                                   ? s_dispatch_pending[i].queued_at_us
                                   : 0;
        if (queued_at_us > 0 &&
            (snapshot.oldest_command_since_us == 0 ||
             queued_at_us < snapshot.oldest_command_since_us)) {
            snapshot.oldest_command_since_us = queued_at_us;
        }
    }
    dispatch_queue = s_dispatch_queue;
    dispatch_task = s_dispatch_task;
    lifecycle_mutex = s_lifecycle_mutex;
    portEXIT_CRITICAL(&s_lock);
    if (dispatch_queue) {
        snapshot.pending_command_count = uxQueueMessagesWaiting(dispatch_queue);
    }
    if (dispatch_task) {
        snapshot.command_dispatch_stack_bytes =
            (uint32_t)uxTaskGetStackHighWaterMark(dispatch_task);
    }
    // Supervisor health sampling must not wait behind a networking operation.
    // A successful try-lock still guarantees that the sampled client cannot be
    // destroyed concurrently; -1 means the outbox was temporarily unavailable.
    if (lifecycle_mutex && xSemaphoreTake(lifecycle_mutex, 0) == pdTRUE) {
        portENTER_CRITICAL(&s_lock);
        esp_mqtt_client_handle_t client = s_client;
        portEXIT_CRITICAL(&s_lock);
        if (client) {
            snapshot.outbox_bytes = esp_mqtt_client_get_outbox_size(client);
        }
        xSemaphoreGive(lifecycle_mutex);
    } else if (client_exists) {
        snapshot.outbox_bytes = -1;
    }
    return snapshot;
}
