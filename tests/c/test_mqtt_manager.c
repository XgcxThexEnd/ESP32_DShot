/* Exercise the real event handler, queue dispatch, and session commit fence. */
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include "command_executor.h"
#include "motor_control.h"
#include "ota_manager.h"
#include "safety_supervisor.h"
#include "state_publisher.h"
#include "tach_actions.h"
#include "../../main/mqtt_manager.c"

struct scheduler_test_semaphore { bool available; };
struct test_client { esp_mqtt_client_connection_state_t state; };
struct test_queue {
    unsigned count;
    mqtt_dispatch_message_t items[MQTT_MANAGER_COMMAND_QUEUE_DEPTH];
};
static int64_t now_us = 1000;
static int subscription_id;
static unsigned callbacks, destroyed;
static jmp_buf dispatch_idle;
static bool running_dispatch;
static bool output_fence, disconnect_before_commit;
static app_config_t config = {.fan_count = 1, .fan_index_start = 1,
    .min_spin_pct = 10, .node_id = "test", .node_topic = "test/nodes/test"};

esp_err_t motor_control_request_manual(int fan, uint8_t target, motor_control_ack_t *ack)
{
    assert(fan == 0 && output_fence && !s_command_commit_mutex->available);
    ++callbacks;
    *ack = (motor_control_ack_t){.accepted = true, .requested_pct = target,
        .accepted_command_count = callbacks};
    return ESP_OK;
}
esp_err_t safety_supervisor_output_command_begin(TickType_t timeout)
{ (void)timeout; assert(!output_fence); output_fence = true; return ESP_OK; }
void safety_supervisor_output_command_end(void) { assert(output_fence); output_fence = false; }
bool safety_supervisor_enable_output_if_safe(void) { assert(output_fence); return true; }
void fan_state_store_request_save(void) { assert(output_fence); }
void state_publisher_request_fan(int fan, bool full)
{ (void)fan; (void)full; assert(!output_fence && s_command_commit_mutex->available); }
bool state_publisher_publish_manual_ack(int fan, uint32_t count, uint8_t requested, uint8_t applied)
{ (void)fan; (void)count; (void)requested; (void)applied; assert(s_command_commit_mutex->available); return true; }
esp_err_t tach_actions_clear_begin(void) { return ESP_OK; }
esp_err_t tach_actions_clear_locked(int fan) { (void)fan; return ESP_OK; }
void tach_actions_clear_end(void) {}
esp_err_t scheduler_set_override(bool inhibited) { (void)inhibited; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t scheduler_set_entry(int fan, int slot, uint32_t interval, uint32_t duration, uint8_t target)
{ (void)fan; (void)slot; (void)interval; (void)duration; (void)target; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t scheduler_disable_entry(int fan, int slot)
{ (void)fan; (void)slot; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t scheduler_get_entry(int fan, int slot, scheduler_entry_snapshot_t *out)
{ (void)fan; (void)slot; (void)out; return ESP_ERR_NOT_SUPPORTED; }
bool state_publisher_publish_schedule_entry(int fan, unsigned slot, const scheduler_entry_snapshot_t *entry)
{ (void)fan; (void)slot; (void)entry; return false; }
ota_manager_start_result_t ota_manager_start(const char *url)
{ (void)url; return OTA_MANAGER_START_DISABLED; }
ota_manager_start_result_t ota_manager_complete_start(ota_manager_start_result_t result)
{ return result; }

void scheduler_test_enter_critical(portMUX_TYPE *lock)
{ assert(!lock->locked); lock->locked = 1; }
void scheduler_test_exit_critical(portMUX_TYPE *lock)
{ assert(lock->locked); lock->locked = 0; }
void scheduler_test_log(const char *tag, const char *format, ...)
{ (void)tag; (void)format; }
int64_t esp_timer_get_time(void) { return now_us; }
esp_err_t esp_crt_bundle_attach(void *conf) { (void)conf; return ESP_OK; }
const app_config_t *app_config_get(void) { return &config; }
bool app_config_format_node_topic(char *out, size_t size, const char *format, ...)
{
    va_list args; va_start(args, format);
    int written = vsnprintf(out, size, format, args); va_end(args);
    return written > 0 && (size_t)written < size;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    SemaphoreHandle_t mutex = calloc(1, sizeof(*mutex));
    assert(mutex); mutex->available = true; return mutex;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t ticks)
{
    if (!mutex->available) { assert(ticks != portMAX_DELAY); return pdFALSE; }
    mutex->available = false; return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex)
{ assert(!mutex->available); mutex->available = true; return pdTRUE; }
void vSemaphoreDelete(SemaphoreHandle_t mutex) { free(mutex); }
QueueHandle_t xQueueCreate(UBaseType_t count, UBaseType_t size)
{
    assert(count == MQTT_MANAGER_COMMAND_QUEUE_DEPTH && size == sizeof(mqtt_dispatch_message_t));
    return calloc(1, sizeof(struct test_queue));
}
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait)
{
    assert(wait == 0);
    if (queue->count == MQTT_MANAGER_COMMAND_QUEUE_DEPTH) return pdFALSE;
    queue->items[queue->count++] = *(const mqtt_dispatch_message_t *)item; return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait)
{
    (void)wait;
    if (!queue->count) { assert(running_dispatch); longjmp(dispatch_idle, 1); }
    *(mqtt_dispatch_message_t *)item = queue->items[0];
    --queue->count;
    memmove(queue->items, queue->items + 1, queue->count * sizeof(queue->items[0]));
    return pdTRUE;
}
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue) { return queue->count; }
void vQueueDelete(QueueHandle_t queue) { free(queue); }
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t depth,
                       void *arg, UBaseType_t priority, TaskHandle_t *task)
{ (void)fn; (void)name; (void)depth; (void)arg; (void)priority; *task = &callbacks; return pdPASS; }
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task) { (void)task; return 1024; }
void vTaskDelay(TickType_t ticks) { now_us += (int64_t)ticks * 1000; }
esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *cfg)
{ assert(cfg); return calloc(1, sizeof(struct test_client)); }
esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t client)
{ ++destroyed; free(client); return ESP_OK; }
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t client)
{ client->state = MQTT_CLIENT_STATE_WAITING_RECONNECT; return ESP_OK; }
esp_err_t esp_mqtt_client_disconnect(esp_mqtt_client_handle_t client)
{ client->state = MQTT_CLIENT_STATE_DISCONNECTED; return ESP_OK; }
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t client, int event,
                                       esp_event_handler_t handler, void *context)
{ (void)client; (void)event; (void)handler; (void)context; return ESP_OK; }
esp_mqtt_client_connection_state_t esp_mqtt_client_get_state(esp_mqtt_client_handle_t client)
{ return client->state; }
int esp_mqtt_client_get_outbox_size(esp_mqtt_client_handle_t client) { (void)client; return 0; }
int esp_mqtt_client_subscribe_single(esp_mqtt_client_handle_t client, const char *topic, int qos)
{ (void)client; assert(topic && qos == 1); return ++subscription_id; }
int esp_mqtt_client_publish(esp_mqtt_client_handle_t client, const char *topic,
                            const char *payload, int len, int qos, int retain)
{ (void)client; (void)topic; (void)payload; (void)len; (void)qos; (void)retain; return 1; }
static void on_message(void *context, const char *topic, const char *data, size_t length,
                        bool retained, const mqtt_manager_command_session_t *session)
{
    if (disconnect_before_commit) {
        esp_mqtt_event_t ev = {.client = s_client};
        mqtt_manager_event_handler(NULL, NULL, MQTT_EVENT_DISCONNECTED, &ev);
    }
    command_executor_on_message(context, topic, data, length, retained, session);
}
static void event(int id, esp_mqtt_event_t *ev)
{ mqtt_manager_event_handler(NULL, NULL, id, ev); }
static void connect_ready(void)
{
    esp_mqtt_event_t ev = {.client = s_client};
    event(MQTT_EVENT_CONNECTED, &ev);
    assert(!mqtt_manager_ready() && s_pending_sub_count > 0);
    char granted = 1;
    ev.data = &granted; ev.data_len = 1;
    while (s_pending_sub_count) {
        ev.msg_id = s_pending_sub_ids[0];
        now_us += 1000; event(MQTT_EVENT_SUBSCRIBED, &ev);
    }
    assert(mqtt_manager_ready());
}
static void drain_dispatch(void)
{
    running_dispatch = true;
    if (setjmp(dispatch_idle) == 0) mqtt_manager_dispatch_task(s_dispatch_queue);
    running_dispatch = false;
}
static void test_rx_cannot_renew_acknowledgement_lease(void)
{
    int64_t baseline = mqtt_manager_health_snapshot().last_ack_us;
    esp_mqtt_event_t ev = {.client = s_client, .topic = "test/nodes/test/fan1/set",
        .topic_len = sizeof("test/nodes/test/fan1/set") - 1,
        .data = "ON", .data_len = 2, .total_data_len = 2};
    now_us += 1000000; event(MQTT_EVENT_DATA, &ev);
    drain_dispatch(); assert(callbacks == 1);
    ev.total_data_len = -1;
    now_us += 1000000; event(MQTT_EVENT_DATA, &ev);
    mqtt_manager_health_t health = mqtt_manager_health_snapshot();
    assert(health.last_ack_us == baseline && health.last_rx_us == now_us);
    event(MQTT_EVENT_PUBLISHED, &ev);
    assert(mqtt_manager_health_snapshot().last_ack_us == now_us);
}
static void test_disconnect_discards_queued_commands_and_fences_old_sessions(void)
{
    unsigned before = callbacks;
    mqtt_manager_command_session_t old = {.connection_generation = s_connection_generation};
    esp_mqtt_event_t ev = {.client = s_client, .topic = "test/nodes/test/fan1/set",
        .topic_len = sizeof("test/nodes/test/fan1/set") - 1,
        .data = "ON", .data_len = 2, .total_data_len = 2};
    event(MQTT_EVENT_DATA, &ev); assert(s_dispatch_queue->count == 1);
    event(MQTT_EVENT_DISCONNECTED, &ev);
    connect_ready(); drain_dispatch();
    assert(callbacks == before);
    assert(mqtt_manager_command_commit_begin(&old, 10) == ESP_ERR_INVALID_STATE);
}
static void test_old_client_events_cannot_renew_lease(void)
{
    struct test_client obsolete = {0};
    esp_mqtt_event_t ev = {.client = &obsolete};
    int64_t baseline = s_last_ack_us;
    now_us += 1000000; event(MQTT_EVENT_PUBLISHED, &ev); event(MQTT_EVENT_DATA, &ev);
    assert(s_last_ack_us == baseline);
}
static void test_disconnect_between_dispatch_and_commit_rejects_motor_mutation(void)
{
    unsigned before = callbacks;
    esp_mqtt_event_t ev = {.client = s_client, .topic = "test/nodes/test/fan1/set",
        .topic_len = sizeof("test/nodes/test/fan1/set") - 1,
        .data = "ON", .data_len = 2, .total_data_len = 2};
    event(MQTT_EVENT_DATA, &ev);
    disconnect_before_commit = true;
    drain_dispatch();
    disconnect_before_commit = false;
    assert(callbacks == before && !output_fence);
    connect_ready();
}
static void test_stopped_client_is_rebuilt_after_grace(void)
{
    esp_mqtt_event_t ev = {.client = s_client}; event(MQTT_EVENT_DISCONNECTED, &ev);
    s_client->state = MQTT_CLIENT_STATE_DISCONNECTED;
    assert(mqtt_manager_start() == ESP_ERR_INVALID_STATE);
    now_us += MQTT_MANAGER_STOPPED_REBUILD_GRACE_US + 1;
    assert(mqtt_manager_start() == ESP_OK && destroyed == 1);
    connect_ready();
}
int main(void)
{
    assert(command_executor_init(&config) == ESP_OK);
    assert(mqtt_manager_init(on_message, NULL) == ESP_OK);
    assert(mqtt_manager_start() == ESP_OK);
    connect_ready();
    test_rx_cannot_renew_acknowledgement_lease();
    test_disconnect_discards_queued_commands_and_fences_old_sessions();
    test_old_client_events_cannot_renew_lease();
    test_disconnect_between_dispatch_and_commit_rejects_motor_mutation();
    test_stopped_client_is_rebuilt_after_grace();
    esp_mqtt_client_destroy(s_client);
    vQueueDelete(s_dispatch_queue);
    vSemaphoreDelete(s_command_commit_mutex); vSemaphoreDelete(s_lifecycle_mutex);
    puts("mqtt_manager + command_executor: 5 tests passed");
    return 0;
}
