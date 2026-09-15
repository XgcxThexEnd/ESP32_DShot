/* Production serialization and retained-state lifecycle, with a fake broker. */
#include <assert.h>
#define CONFIG_HOME_ASSISTANT_DISCOVERY_ENABLED 1
#include "../../main/state_publisher.c"

typedef struct { char topic[256], payload[2048]; int qos; bool retained; } message_t;
static message_t messages[64];
static unsigned message_count;
static int64_t now_us = 2000000;
static tach_monitor_snapshot_t tach = {.measurement_valid = true,
    .measured_rpm = 1200, .sampled_at_us = 1800000};
static app_config_t config = {.fan_count = 1, .fan_index_start = 1,
    .node_id = "test", .node_topic = "test/nodes/test"};
void scheduler_test_enter_critical(portMUX_TYPE *lock)
{ assert(!lock->locked); lock->locked = 1; }
void scheduler_test_exit_critical(portMUX_TYPE *lock)
{ assert(lock->locked); lock->locked = 0; }
void scheduler_test_log(const char *tag, const char *format, ...)
{ (void)tag; (void)format; }
int64_t esp_timer_get_time(void) { return now_us; }
uint32_t esp_get_free_heap_size(void) { return 12345; }
uint32_t esp_get_minimum_free_heap_size(void) { return 1234; }
int esp_reset_reason(void) { return 1; }
const esp_app_desc_t *esp_app_get_description(void) { return NULL; }
const esp_partition_t *esp_ota_get_running_partition(void) { return NULL; }
bool mqtt_manager_ready(void) { return true; }
bool app_config_format_node_topic(char *out, size_t size, const char *format, ...)
{
    va_list args; va_start(args, format);
    int n = snprintf(out, size, "%s/", config.node_topic);
    assert(n > 0 && (size_t)n < size);
    int m = vsnprintf(out + n, size - (size_t)n, format, args); va_end(args);
    return m >= 0 && (size_t)m < size - (size_t)n;
}
bool mqtt_manager_publish(const char *topic, const char *payload, int qos, bool retain)
{
    assert(message_count < 64);
    message_t *message = &messages[message_count++];
    assert(strlen(topic) < sizeof(message->topic) && strlen(payload) < sizeof(message->payload));
    strcpy(message->topic, topic); strcpy(message->payload, payload);
    message->qos = qos; message->retained = retain; return true;
}
bool motor_control_get_fan_snapshot(int fan, motor_control_fan_snapshot_t *out)
{ assert(fan == 0); *out = (motor_control_fan_snapshot_t){0}; return true; }
UBaseType_t motor_control_ramp_stack_bytes(void) { return 1024; }
esp_err_t tach_monitor_get_snapshot(int fan, tach_monitor_snapshot_t *out)
{ assert(fan == 0); *out = tach; return ESP_OK; }
fan_state_store_health_t fan_state_store_health_snapshot(void)
{ return (fan_state_store_health_t){0}; }
mqtt_manager_health_t mqtt_manager_health_snapshot(void)
{ return (mqtt_manager_health_t){.last_ack_us = 1000000, .last_rx_us = 1500000,
    .command_dispatch_stack_bytes = 2048}; }
void wifi_manager_get_snapshot(wifi_manager_snapshot_t *out)
{ *out = (wifi_manager_snapshot_t){0}; }
BaseType_t xTaskNotify(TaskHandle_t task, uint32_t bits, eNotifyAction action)
{ (void)task; (void)bits; (void)action; return pdTRUE; }
BaseType_t xTaskNotifyWait(uint32_t entry, uint32_t exit_bits, uint32_t *bits, TickType_t ticks)
{ (void)entry; (void)exit_bits; (void)bits; (void)ticks; assert(false); return pdFALSE; }
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack,
                       void *arg, UBaseType_t priority, TaskHandle_t *task)
{ (void)fn; (void)name; (void)stack; (void)arg; (void)priority; *task = &now_us; return pdPASS; }
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task) { (void)task; return 2048; }
static const message_t *find_message(const char *suffix)
{
    for (unsigned i = 0; i < message_count; ++i) {
        const char *topic = messages[i].topic;
        size_t prefix = strlen(topic) - strlen(suffix);
        if (strcmp(topic + prefix, suffix) == 0) return &messages[i];
    }
    assert(false); return NULL;
}
static void expect_leaf(const char *leaf, const char *value, bool retained)
{
    const message_t *message = find_message(leaf);
    assert(strcmp(message->payload, value) == 0 && message->retained == retained);
}
int main(void)
{
    state_publisher_config_t cfg = {.app_config = &config};
    assert(state_publisher_init(&cfg) == ESP_OK);
    publish_one_state(0, 1, true);
    expect_leaf("/measured_rpm", "1200", true);
    expect_leaf("/rpm_status", "online", true);
    expect_leaf("/tach_sample_age_ms", "200", true);

    message_count = 0; tach.measurement_valid = false;
    publish_one_state(0, 0, false); /* Even transient updates must clear retained RPM. */
    expect_leaf("/measured_rpm", "", true);
    expect_leaf("/rpm_status", "offline", true);
    expect_leaf("/tach_valid", "0", false);

    message_count = 0; tach.measurement_valid = true; tach.sampled_at_us = 1;
    publish_one_state(0, 1, true);
    expect_leaf("/rpm_status", "offline", true);

    message_count = 0; tach.sampled_at_us = now_us;
    publish_one_state(0, 1, true);
    expect_leaf("/rpm_status", "online", true);
    expect_leaf("/measured_rpm", "1200", true);

    message_count = 0; publish_discovery();
    const message_t *discovery = find_message("_rpm/config");
    assert(strstr(discovery->payload, "\"availability_mode\":\"all\""));
    assert(strstr(discovery->payload, "test/nodes/test/status"));
    assert(strstr(discovery->payload, "test/nodes/test/fan1/rpm_status"));

    message_count = 0; publish_health_probe();
    const message_t *health = find_message("/health");
    assert(strstr(health->payload, "\"mqtt_ack_age_ms\":1000"));
    assert(strstr(health->payload, "\"mqtt_rx_age_ms\":500"));
    assert(strstr(health->payload, "\"mqtt_dispatch_stack_bytes\":2048"));
    assert(!strstr(health->payload, "stack_words"));
    puts("state_publisher: valid, invalid, stale, recovered RPM, discovery, health passed");
    return 0;
}
