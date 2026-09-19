/*
 * Exercise the production scheduler with deterministic task preemption at
 * mutex releases and NVS commits. No firmware-only test hooks are needed.
 */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../main/scheduler.c"

struct scheduler_test_semaphore {
    bool available;
    bool mutex;
};

static TickType_t test_ticks;
static uint32_t test_notifications;
static int test_task_tokens[3];
static TaskHandle_t test_current_task;
static jmp_buf test_writer_idle;
static bool test_writer_running;
static bool test_sync_preempts;
static void (*test_unlock_hook)(void);
static void (*test_commit_hook)(void);
static schedule_state_blob_t test_pending_blob;
static schedule_state_blob_t test_committed_blob;
static bool test_blob_present;
static unsigned test_commit_count;
static unsigned test_override_events;
static esp_err_t test_next_commit_error;
static app_config_t test_config;

void scheduler_test_enter_critical(portMUX_TYPE *lock)
{
    assert(!lock->locked);
    lock->locked = 1;
}

void scheduler_test_exit_critical(portMUX_TYPE *lock)
{
    assert(lock->locked);
    lock->locked = 0;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    SemaphoreHandle_t semaphore = calloc(1, sizeof(*semaphore));
    assert(semaphore);
    semaphore->mutex = true;
    semaphore->available = true;
    return semaphore;
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    SemaphoreHandle_t semaphore = calloc(1, sizeof(*semaphore));
    assert(semaphore);
    return semaphore;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks)
{
    assert(semaphore);
    if (semaphore->available) {
        semaphore->available = false;
        return pdTRUE;
    }
    /* An unavailable mutex here would deadlock the real task as well. */
    assert(ticks != portMAX_DELAY);
    test_ticks += ticks;
    return pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    assert(semaphore);
    if (semaphore->mutex) assert(!semaphore->available);
    semaphore->available = true;
    if (semaphore == s_mutex && test_unlock_hook) {
        void (*hook)(void) = test_unlock_hook;
        test_unlock_hook = NULL;
        hook();
    }
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    free(semaphore);
}

TickType_t xTaskGetTickCount(void)
{
    return test_ticks;
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return test_current_task;
}

BaseType_t xTaskNotify(TaskHandle_t task, uint32_t value, eNotifyAction action)
{
    assert(task == s_save_task && action == eSetBits);
    test_notifications |= value;
    if ((value & SAVE_NOTIFY_SYNC) && test_sync_preempts) {
        TaskHandle_t previous = test_current_task;
        test_current_task = s_save_task;
        process_pending_sync_saves();
        test_current_task = previous;
    }
    return pdTRUE;
}

BaseType_t xTaskNotifyWait(uint32_t clear_on_entry, uint32_t clear_on_exit,
                           uint32_t *value, TickType_t ticks)
{
    assert(test_writer_running && test_current_task == s_save_task);
    assert(clear_on_entry == 0);
    if (test_notifications) {
        *value = test_notifications;
        test_notifications &= ~clear_on_exit;
        return pdTRUE;
    }
    if (ticks == portMAX_DELAY) longjmp(test_writer_idle, 1);
    test_ticks += ticks;
    *value = 0;
    return pdFALSE;
}

BaseType_t xTaskCreate(TaskFunction_t function, const char *name,
                        uint32_t stack_depth, void *argument,
                        UBaseType_t priority, TaskHandle_t *task)
{
    (void)name;
    (void)stack_depth;
    (void)argument;
    (void)priority;
    assert(function == save_task_main);
    *task = &test_task_tokens[1];
    return pdPASS;
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t function, const char *name,
                                    uint32_t stack_depth, void *argument,
                                    UBaseType_t priority, TaskHandle_t *task,
                                    BaseType_t core)
{
    (void)name;
    (void)stack_depth;
    (void)argument;
    (void)priority;
    (void)core;
    assert(function == scheduler_task_main);
    *task = &test_task_tokens[2];
    return pdPASS;
}

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task)
{
    (void)task;
    return 1024;
}

void vTaskDelay(TickType_t ticks)
{
    test_ticks += ticks;
}

void vTaskDelete(TaskHandle_t task)
{
    (void)task;
}

int64_t esp_timer_get_time(void)
{
    return (int64_t)test_ticks * 1000;
}

void scheduler_test_log(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}

const char *esp_err_to_name(esp_err_t error)
{
    (void)error;
    return "test error";
}

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{
    assert(strcmp(name, SCHEDULE_NAMESPACE) == 0);
    if (mode == NVS_READONLY && !test_blob_present) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *handle = 1;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value,
                       size_t *length)
{
    assert(handle == 1 && strcmp(key, SCHEDULE_NVS_KEY) == 0);
    assert(test_blob_present && *length == sizeof(test_committed_blob));
    memcpy(value, &test_committed_blob, sizeof(test_committed_blob));
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value,
                       size_t length)
{
    assert(handle == 1 && strcmp(key, SCHEDULE_NVS_KEY) == 0);
    assert(length == sizeof(test_pending_blob));
    assert(s_mutex->available);
    memcpy(&test_pending_blob, value, length);
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1);
    ++test_commit_count;
    esp_err_t result = test_next_commit_error;
    test_next_commit_error = ESP_OK;
    if (result == ESP_OK) {
        test_committed_blob = test_pending_blob;
        test_blob_present = true;
    }
    if (test_commit_hook) {
        TaskHandle_t previous = test_current_task;
        test_current_task = &test_task_tokens[0];
        test_commit_hook();
        test_current_task = previous;
    }
    return result;
}

void nvs_close(nvs_handle_t handle)
{
    assert(handle == 1);
}

static void test_apply_target(const scheduler_target_request_t *request,
                               scheduler_target_result_t *result,
                               void *context)
{
    (void)context;
    assert(s_mutex->available);
    result->accepted = true;
    result->manual_target_pct = request->target_pct;
}

static void test_notify(const scheduler_event_t *event, void *context)
{
    (void)context;
    if (event->type == SCHEDULER_EVENT_OVERRIDE_CHANGED) ++test_override_events;
}

static void reset_scheduler(bool preserve_storage)
{
    if (s_mutex) vSemaphoreDelete(s_mutex);
    if (s_sync_mutex) vSemaphoreDelete(s_sync_mutex);
    if (s_sync_complete) vSemaphoreDelete(s_sync_complete);
    s_mutex = NULL;
    s_sync_mutex = NULL;
    s_sync_complete = NULL;
    memset(&s_config, 0, sizeof(s_config));
    memset(s_fans, 0, sizeof(s_fans));
    s_scheduler_task = NULL;
    s_save_task = NULL;
    s_init_started = false;
    s_initialized = false;
    s_start_started = false;
    s_scheduler_task_ready = false;
    s_save_task_ready = false;
    s_inhibited = false;
    s_runtime_inhibit_latched = false;
    s_heartbeat_us = 0;
    s_sync_requested_generation = 0;
    s_sync_completed_generation = 0;
    s_sync_result = ESP_ERR_INVALID_STATE;
    test_ticks = 1000;
    test_notifications = 0;
    test_current_task = &test_task_tokens[0];
    test_writer_running = false;
    test_sync_preempts = false;
    test_unlock_hook = NULL;
    test_commit_hook = NULL;
    test_commit_count = 0;
    test_override_events = 0;
    test_next_commit_error = ESP_OK;
    if (!preserve_storage) {
        test_blob_present = false;
        memset(&test_committed_blob, 0, sizeof(test_committed_blob));
    }
    test_config = (app_config_t) {
        .fan_count = APP_CONFIG_MAX_FANS,
        .fan_index_start = 5,
        .topology_fingerprint = 12345,
    };
    scheduler_config_t config = {
        .app_config = &test_config,
        .apply_target = test_apply_target,
        .notify = test_notify,
    };
    assert(scheduler_init(&config) == ESP_OK);
}

static void run_writer_until_idle(void)
{
    test_writer_running = true;
    test_current_task = s_save_task;
    if (setjmp(test_writer_idle) == 0) save_task_main(NULL);
    test_current_task = &test_task_tokens[0];
    test_writer_running = false;
    assert(s_mutex->available);
}

static void assert_inhibited(bool expected)
{
    bool inhibited = !expected;
    assert(scheduler_get_override(&inhibited) == ESP_OK);
    assert(inhibited == expected);
}

static void safety_stop_after_early_check(void)
{
    assert(!s_runtime_inhibit_latched);
    test_sync_preempts = true;
    assert(scheduler_try_inhibit_all(pdMS_TO_TICKS(1000)) == ESP_OK);
    test_sync_preempts = false;
    assert(test_committed_blob.inhibited == 1);
}

static void test_safety_stop_cannot_be_cleared_by_racing_override(void)
{
    reset_scheduler(false);
    assert(scheduler_set_entry(0, 0, 15, 2, 45) == ESP_OK);
    assert(scheduler_start() == ESP_OK);
    int64_t original_trigger = s_fans[0].entries[0].next_trigger_us;

    /* Preempt OFF just after it releases the mutex protecting its early check. */
    test_unlock_hook = safety_stop_after_early_check;
    assert(scheduler_set_override(false) == ESP_ERR_INVALID_STATE);
    assert_inhibited(true);
    assert(s_runtime_inhibit_latched);
    assert(test_override_events == 0);
    assert(s_fans[0].entries[0].next_trigger_us == original_trigger);
    for (int fan = 0; fan < test_config.fan_count; ++fan) {
        assert(!s_fans[fan].operation_pending);
        assert(scheduler_set_entry(fan, 1, 30, 3, 50) == ESP_OK);
    }
    run_writer_until_idle();
    assert(test_committed_blob.inhibited == 1);

    /* The safety stop must also survive a reboot and a subsequent save. */
    reset_scheduler(true);
    assert_inhibited(true);
}

static void test_override_clears_only_unlatched_inhibit(void)
{
    reset_scheduler(false);
    assert(scheduler_start() == ESP_OK);
    assert(scheduler_set_override(true) == ESP_OK);
    assert(scheduler_set_override(false) == ESP_OK);
    assert_inhibited(false);
    assert(test_override_events == 2);

    test_sync_preempts = true;
    assert(scheduler_inhibit_all() == ESP_OK);
    test_sync_preempts = false;
    assert(scheduler_set_override(false) == ESP_ERR_INVALID_STATE);
    assert_inhibited(true);
    assert(test_override_events == 2);
}

static void request_sync_then_edit_during_commits(void)
{
    if (test_commit_count == 1) {
        /* Writer observes generation 2 in its loop before consuming its bit. */
        assert(scheduler_save_sync(0) == ESP_ERR_TIMEOUT);
    } else if (test_commit_count == 2) {
        /* This edit is newer than both snapshots, and joins the stale SYNC bit. */
        assert(scheduler_set_entry(0, 0, 15, 2, 73) == ESP_OK);
    }
}

static void test_stale_sync_cannot_drop_async_edit(bool initial_async)
{
    reset_scheduler(false);
    if (initial_async) assert(scheduler_start() == ESP_OK);
    assert(scheduler_set_entry(0, 0, 15, 2, 10) == ESP_OK);
    if (!initial_async) assert(scheduler_start() == ESP_OK);

    assert(scheduler_save_sync(0) == ESP_ERR_TIMEOUT);
    test_commit_hook = request_sync_then_edit_during_commits;
    run_writer_until_idle();
    assert(test_commit_count == 3);
    assert(test_committed_blob.schedules[0][0].target_pct == 73);
    assert(s_sync_requested_generation == s_sync_completed_generation);
    assert(test_notifications == 0);

    reset_scheduler(true);
    scheduler_entry_snapshot_t snapshot;
    assert(scheduler_get_entry(0, 0, &snapshot) == ESP_OK);
    assert(snapshot.enabled && snapshot.target_pct == 73);
}

static void test_async_retries_after_failed_sync_snapshot(void)
{
    reset_scheduler(false);
    assert(scheduler_start() == ESP_OK);
    assert(scheduler_set_entry(0, 0, 15, 2, 61) == ESP_OK);
    assert(scheduler_save_sync(0) == ESP_ERR_TIMEOUT);
    test_next_commit_error = ESP_FAIL;
    run_writer_until_idle();
    assert(test_commit_count == 2);
    assert(s_sync_result == ESP_FAIL);
    assert(test_committed_blob.schedules[0][0].target_pct == 61);
}

static void test_async_burst_is_debounced(void)
{
    reset_scheduler(false);
    assert(scheduler_start() == ESP_OK);
    assert(scheduler_set_entry(0, 0, 15, 2, 10) == ESP_OK);
    assert(scheduler_set_entry(0, 0, 15, 2, 20) == ESP_OK);
    assert(scheduler_set_entry(1, 0, 30, 3, 30) == ESP_OK);
    run_writer_until_idle();
    assert(test_commit_count == 1);
    assert(test_committed_blob.schedules[0][0].target_pct == 20);
    assert(test_committed_blob.schedules[1][0].target_pct == 30);
}

int main(void)
{
    test_safety_stop_cannot_be_cleared_by_racing_override();
    test_override_clears_only_unlatched_inhibit();
    test_stale_sync_cannot_drop_async_edit(false);
    test_stale_sync_cannot_drop_async_edit(true);
    test_async_retries_after_failed_sync_snapshot();
    test_async_burst_is_debounced();
    vSemaphoreDelete(s_mutex);
    vSemaphoreDelete(s_sync_mutex);
    vSemaphoreDelete(s_sync_complete);
    puts("scheduler tests passed (6 cases)");
    return 0;
}
