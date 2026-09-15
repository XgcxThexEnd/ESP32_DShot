/* Production sampling and fault state with deterministic PCNT read failures. */
#include <assert.h>
#include <stdio.h>
#ifdef _MSC_VER
/* Production Kconfig intentionally makes the filter-enable condition constant. */
#pragma warning(disable: 4127)
#endif
#include "../../main/tach_monitor.c"

static int counter;
static esp_err_t read_error, clear_error, start_error, stop_error;
static unsigned faults;
static tach_monitor_stall_event_t last_fault;
static app_config_t config = {.fan_count = 1, .fan_index_start = 1};

void scheduler_test_enter_critical(portMUX_TYPE *lock)
{ assert(!lock->locked); lock->locked = 1; }
void scheduler_test_exit_critical(portMUX_TYPE *lock)
{ assert(lock->locked); lock->locked = 0; }
void scheduler_test_log(const char *tag, const char *format, ...)
{ (void)tag; (void)format; }
const char *esp_err_to_name(esp_err_t error) { (void)error; return "test error"; }
int64_t esp_timer_get_time(void) { return 10000000; }
void vTaskDelay(TickType_t ticks) { (void)ticks; }
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task) { (void)task; return 512; }
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t depth,
                       void *arg, UBaseType_t priority, TaskHandle_t *task)
{ (void)fn; (void)name; (void)depth; (void)arg; (void)priority; *task = &counter; return pdPASS; }
esp_err_t gpio_set_pull_mode(int gpio, gpio_pull_mode_t mode)
{ (void)gpio; (void)mode; return ESP_OK; }
esp_err_t pcnt_new_unit(const pcnt_unit_config_t *cfg, pcnt_unit_handle_t *unit)
{ assert(cfg->flags.accum_count); *unit = &counter; return ESP_OK; }
esp_err_t pcnt_new_channel(pcnt_unit_handle_t unit, const pcnt_chan_config_t *cfg,
                           pcnt_channel_handle_t *channel)
{ (void)cfg; *channel = unit; return ESP_OK; }
esp_err_t pcnt_unit_set_glitch_filter(pcnt_unit_handle_t unit,
                                     const pcnt_glitch_filter_config_t *filter)
{ (void)unit; (void)filter; return ESP_OK; }
esp_err_t pcnt_channel_set_edge_action(pcnt_channel_handle_t channel, int pos, int neg)
{ (void)channel; (void)pos; (void)neg; return ESP_OK; }
esp_err_t pcnt_unit_add_watch_point(pcnt_unit_handle_t unit, int point)
{ (void)unit; (void)point; return ESP_OK; }
esp_err_t pcnt_unit_enable(pcnt_unit_handle_t unit) { (void)unit; return ESP_OK; }
esp_err_t pcnt_unit_disable(pcnt_unit_handle_t unit) { (void)unit; return ESP_OK; }
esp_err_t pcnt_del_unit(pcnt_unit_handle_t unit) { (void)unit; return ESP_OK; }
esp_err_t pcnt_del_channel(pcnt_channel_handle_t channel) { (void)channel; return ESP_OK; }
esp_err_t pcnt_unit_start(pcnt_unit_handle_t unit) { (void)unit; return start_error; }
esp_err_t pcnt_unit_stop(pcnt_unit_handle_t unit) { (void)unit; return stop_error; }
esp_err_t pcnt_unit_clear_count(pcnt_unit_handle_t unit)
{ (void)unit; if (clear_error == ESP_OK) counter = 0; return clear_error; }
esp_err_t pcnt_unit_get_count(pcnt_unit_handle_t unit, int *count)
{ (void)unit; *count = counter; return read_error; }

static bool motor_snapshot(int fan, tach_monitor_motor_snapshot_t *out, void *ctx)
{
    (void)ctx; assert(fan == 0);
    *out = (tach_monitor_motor_snapshot_t){.applied_pct = 50, .nonzero_applied_since_us = 1};
    return true;
}
static void on_fault(const tach_monitor_stall_event_t *event, void *ctx)
{
    (void)ctx; assert(!s_state_lock.locked);
    faults++; last_fault = *event;
}
static void reset_fixture(void)
{
    assert(release_pcnt_resources() == ESP_OK);
    memset(s_fans, 0, sizeof(s_fans));
    s_init_started = s_initialized = s_start_started = false;
    s_task = NULL;
    read_error = clear_error = start_error = stop_error = ESP_OK;
    faults = 0;
    tach_monitor_config_t cfg = {.app_config = &config,
        .read_motor_snapshot = motor_snapshot, .confirmed_stall = on_fault};
    assert(tach_monitor_init(&cfg) == ESP_OK);
    counter = 100;
    sample_fan(0, 2000000, 500000); /* Establish the first complete interval. */
    assert(!s_fans[0].measurement_valid);
}
static void expect_sample(int count, int64_t now, bool valid, uint32_t rpm)
{
    counter = count;
    sample_fan(0, now, 500000);
    tach_monitor_snapshot_t snapshot;
    assert(tach_monitor_get_snapshot(0, &snapshot) == ESP_OK);
    assert(snapshot.measurement_valid == valid);
    assert(snapshot.measured_rpm == rpm);
    assert(snapshot.sampled_at_us == now);
}
static void test_read_failure_rebases_before_rpm_resumes(void)
{
    reset_fixture();
    expect_sample(120, 2500000, true, 1200);
    read_error = ESP_FAIL;
    expect_sample(140, 3000000, false, 0);
    read_error = ESP_OK;
    expect_sample(160, 3500000, false, 0);
    expect_sample(180, 4000000, true, 1200);
    assert(faults == 0);
}
static void test_counter_decrease_rebases(void)
{
    reset_fixture();
    expect_sample(10, 2500000, false, 0);
    expect_sample(30, 3000000, true, 1200);
}
static void test_repeated_invalid_samples_latch_sensor_fault(void)
{
    reset_fixture();
    read_error = ESP_FAIL;
    for (int i = 0; i < 5; ++i) expect_sample(100, 2500000 + i * 500000LL, false, 0);
    assert(faults == 1 && last_fault.cause == TACH_MONITOR_FAULT_SENSOR_INVALID);
    assert(s_fans[0].stall_latched);
    read_error = ESP_OK;
}
static void test_reset_read_failure_does_not_reuse_old_interval(void)
{
    reset_fixture();
    s_fans[0].previous_pulse_count = TACH_COUNTER_RESET_THRESHOLD;
    read_error = ESP_FAIL;
    expect_sample(TACH_COUNTER_RESET_THRESHOLD + 20, 2500000, false, 0);
    read_error = ESP_OK;
    expect_sample(20, 3000000, false, 0);
    expect_sample(40, 3500000, true, 1200);
}
static void test_reset_stop_failure_requires_rebase(void)
{
    reset_fixture();
    s_fans[0].previous_pulse_count = TACH_COUNTER_RESET_THRESHOLD;
    stop_error = ESP_FAIL;
    expect_sample(TACH_COUNTER_RESET_THRESHOLD + 20, 2500000, false, 0);
    stop_error = ESP_OK;
    expect_sample(TACH_COUNTER_RESET_THRESHOLD + 40, 3000000, false, 0);
    expect_sample(20, 3500000, true, 1200);
}
static void test_restart_failure_recovers_without_accumulated_rpm(void)
{
    reset_fixture();
    s_fans[0].previous_pulse_count = TACH_COUNTER_RESET_THRESHOLD;
    start_error = ESP_FAIL;
    expect_sample(TACH_COUNTER_RESET_THRESHOLD + 20, 2500000, false, 0);
    assert(!s_fans[0].unit_started);
    start_error = ESP_OK;
    expect_sample(20, 3000000, false, 0);
    expect_sample(20, 3500000, false, 0);
    expect_sample(40, 4000000, true, 1200);
}
int main(void)
{
    test_read_failure_rebases_before_rpm_resumes();
    test_counter_decrease_rebases();
    test_repeated_invalid_samples_latch_sensor_fault();
    test_reset_read_failure_does_not_reuse_old_interval();
    test_reset_stop_failure_requires_rebase();
    test_restart_failure_recovers_without_accumulated_rpm();
    assert(release_pcnt_resources() == ESP_OK);
    puts("tach_monitor: 6 tests passed");
    return 0;
}
