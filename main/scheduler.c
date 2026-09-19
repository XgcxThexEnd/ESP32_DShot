#include "scheduler.h"

#include <inttypes.h>
#include <stddef.h>
#include <string.h>

#include "sdkconfig.h"

#ifdef CONFIG_SCHEDULE_ENABLED

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#define SCHEDULE_NAMESPACE              "fan_state"
#define SCHEDULE_NVS_KEY                "schedules_v4"
#define SCHEDULE_STATE_MAGIC            0x53434834U
#define SCHEDULE_SCHEMA_VERSION         4U
#define SCHEDULER_TASK_STACK            3072
#define SCHEDULER_TASK_PRIORITY         5
#define SCHEDULER_SAVE_TASK_STACK       3072
#define SCHEDULER_SAVE_TASK_PRIORITY    4
#define SCHEDULER_OPERATION_WAIT_MS     1000U
#define SAVE_NOTIFY_ASYNC               (1UL << 0)
#define SAVE_NOTIFY_SYNC                (1UL << 1)

typedef struct {
    bool enabled;
    uint32_t interval_min;
    uint32_t duration_min;
    uint8_t target_pct;
    int64_t next_trigger_us;
} schedule_entry_t;

/** Keep this entry layout stable within schema version 4. */
typedef struct {
    uint32_t interval_min;
    uint32_t duration_min;
    uint8_t enabled;
    uint8_t target_pct;
    uint8_t reserved[2];
} schedule_entry_persist_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t length;
    uint64_t topology_fingerprint;
    int32_t fan_index_start;
    uint8_t fan_count;
    uint8_t slot_count;
    uint8_t inhibited;
    uint8_t reserved;
    schedule_entry_persist_t
        schedules[APP_CONFIG_MAX_FANS][CONFIG_SCHEDULE_SLOTS];
    uint32_t checksum;
} schedule_state_blob_t;

_Static_assert(sizeof(schedule_entry_persist_t) == 12,
               "schedule entry schema changed");

typedef struct {
    schedule_entry_t entries[CONFIG_SCHEDULE_SLOTS];
    int active_slot;
    int64_t active_end_us;
    uint8_t saved_manual_target;
    uint8_t cursor;
    bool operation_pending;
} schedule_fan_state_t;

static const char *TAG = "scheduler";
static portMUX_TYPE s_lifecycle_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_heartbeat_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_mutex;
static SemaphoreHandle_t s_sync_mutex;
static SemaphoreHandle_t s_sync_complete;
static scheduler_config_t s_config;
static schedule_fan_state_t s_fans[APP_CONFIG_MAX_FANS];
static TaskHandle_t s_scheduler_task;
static TaskHandle_t s_save_task;
static bool s_init_started;
static bool s_initialized;
static bool s_start_started;
static bool s_scheduler_task_ready;
static bool s_save_task_ready;
static bool s_inhibited;
static bool s_runtime_inhibit_latched;
static int64_t s_heartbeat_us;
static uint32_t s_sync_requested_generation;
static uint32_t s_sync_completed_generation;
static esp_err_t s_sync_result = ESP_ERR_INVALID_STATE;

static bool scheduler_is_initialized(void)
{
    portENTER_CRITICAL(&s_lifecycle_lock);
    bool initialized = s_initialized;
    portEXIT_CRITICAL(&s_lifecycle_lock);
    return initialized;
}

static uint32_t schedule_checksum(const void *data, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < length; ++i) {
        hash = (hash ^ bytes[i]) * 16777619U;
    }
    return hash;
}

static bool entry_config_valid(uint32_t interval_min, uint32_t duration_min,
                               uint32_t target_pct)
{
    return interval_min >= 1 && interval_min <= SCHEDULER_MAX_INTERVAL_MIN &&
           duration_min >= 1 && duration_min <= interval_min &&
           target_pct <= 100;
}

static bool local_fan_valid(int local_fan)
{
    return s_config.app_config && local_fan >= 0 &&
           local_fan < s_config.app_config->fan_count;
}

static bool slot_valid(int slot)
{
    return slot >= 0 && slot < CONFIG_SCHEDULE_SLOTS;
}

static void notify_event(const scheduler_event_t *event)
{
    if (event && s_config.notify) {
        s_config.notify(event, s_config.callback_context);
    }
}

static void apply_target(const scheduler_target_request_t *request,
                         scheduler_target_result_t *result)
{
    *result = (scheduler_target_result_t) {0};
    // This call deliberately occurs with no scheduler mutex held.
    s_config.apply_target(request, result, s_config.callback_context);
    if (result->manual_target_pct > 100) result->accepted = false;
}

static bool reserve_fan_operation(int local_fan)
{
    TickType_t wait_ticks = pdMS_TO_TICKS(SCHEDULER_OPERATION_WAIT_MS);
    if (wait_ticks == 0) wait_ticks = 1;
    TickType_t started = xTaskGetTickCount();
    while (true) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (!s_fans[local_fan].operation_pending) {
            s_fans[local_fan].operation_pending = true;
            xSemaphoreGive(s_mutex);
            return true;
        }
        xSemaphoreGive(s_mutex);
        if (xTaskGetTickCount() - started >= wait_ticks) return false;
        vTaskDelay(1);
    }
}

static void release_fan_operation_locked(int local_fan)
{
    s_fans[local_fan].operation_pending = false;
}

static bool reserve_all_fans(void)
{
    int reserved = 0;
    for (; reserved < s_config.app_config->fan_count; ++reserved) {
        if (reserve_fan_operation(reserved)) continue;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        for (int i = 0; i < reserved; ++i) release_fan_operation_locked(i);
        xSemaphoreGive(s_mutex);
        return false;
    }
    return true;
}

static void release_all_fans_locked(void)
{
    for (int i = 0; i < s_config.app_config->fan_count; ++i) {
        release_fan_operation_locked(i);
    }
}

static esp_err_t load_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(SCHEDULE_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open schedules: %s", esp_err_to_name(err));
        return err;
    }

    schedule_state_blob_t saved = {0};
    size_t length = sizeof(saved);
    err = nvs_get_blob(handle, SCHEDULE_NVS_KEY, &saved, &length);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;

    bool valid = err == ESP_OK && length == sizeof(saved) &&
                 saved.magic == SCHEDULE_STATE_MAGIC &&
                 saved.version == SCHEDULE_SCHEMA_VERSION &&
                 saved.length == sizeof(saved) &&
                 saved.topology_fingerprint ==
                     s_config.app_config->topology_fingerprint &&
                 saved.fan_index_start == s_config.app_config->fan_index_start &&
                 saved.fan_count == s_config.app_config->fan_count &&
                 saved.slot_count == CONFIG_SCHEDULE_SLOTS &&
                 saved.inhibited <= 1 &&
                 saved.checksum == schedule_checksum(
                     &saved, offsetof(schedule_state_blob_t, checksum));
    if (!valid) {
        ESP_LOGE(TAG, "Rejected invalid or incompatible schedule-state blob");
        return err == ESP_OK ? ESP_ERR_INVALID_CRC : err;
    }

    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int fan = 0; fan < s_config.app_config->fan_count; ++fan) {
        for (int slot = 0; slot < CONFIG_SCHEDULE_SLOTS; ++slot) {
            const schedule_entry_persist_t *persisted =
                &saved.schedules[fan][slot];
            if (!persisted->enabled) continue;
            if (!entry_config_valid(persisted->interval_min,
                                    persisted->duration_min,
                                    persisted->target_pct)) {
                ESP_LOGW(TAG, "Ignoring invalid persisted schedule fan%d slot%d",
                         s_config.app_config->fan_index_start + fan, slot);
                continue;
            }
            schedule_entry_t *entry = &s_fans[fan].entries[slot];
            entry->enabled = true;
            entry->interval_min = persisted->interval_min;
            entry->duration_min = persisted->duration_min;
            entry->target_pct = persisted->target_pct;
            entry->next_trigger_us =
                now + (int64_t)persisted->interval_min * 60 * 1000000;
        }
    }
    s_inhibited = saved.inhibited != 0;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Loaded validated schedule configuration from NVS");
    return ESP_OK;
}

static esp_err_t save_to_nvs(void)
{
    schedule_state_blob_t saved = {
        .magic = SCHEDULE_STATE_MAGIC,
        .version = SCHEDULE_SCHEMA_VERSION,
        .length = sizeof(schedule_state_blob_t),
        .topology_fingerprint = s_config.app_config->topology_fingerprint,
        .fan_index_start = s_config.app_config->fan_index_start,
        .fan_count = (uint8_t)s_config.app_config->fan_count,
        .slot_count = CONFIG_SCHEDULE_SLOTS,
    };

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    saved.inhibited = s_inhibited ? 1 : 0;
    for (int fan = 0; fan < s_config.app_config->fan_count; ++fan) {
        for (int slot = 0; slot < CONFIG_SCHEDULE_SLOTS; ++slot) {
            const schedule_entry_t *entry = &s_fans[fan].entries[slot];
            saved.schedules[fan][slot] = (schedule_entry_persist_t) {
                .interval_min = entry->interval_min,
                .duration_min = entry->duration_min,
                .enabled = entry->enabled,
                .target_pct = entry->target_pct,
            };
        }
    }
    xSemaphoreGive(s_mutex);

    saved.checksum = schedule_checksum(
        &saved, offsetof(schedule_state_blob_t, checksum));
    nvs_handle_t handle;
    esp_err_t err = nvs_open(SCHEDULE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, SCHEDULE_NVS_KEY, &saved, sizeof(saved));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static bool sync_save_pending(uint32_t *generation)
{
    bool pending;
    portENTER_CRITICAL(&s_lifecycle_lock);
    pending = s_sync_requested_generation != s_sync_completed_generation;
    if (pending) *generation = s_sync_requested_generation;
    portEXIT_CRITICAL(&s_lifecycle_lock);
    return pending;
}

static void complete_sync_save(uint32_t generation, esp_err_t result)
{
    portENTER_CRITICAL(&s_lifecycle_lock);
    s_sync_completed_generation = generation;
    s_sync_result = result;
    portEXIT_CRITICAL(&s_lifecycle_lock);
    xSemaphoreGive(s_sync_complete);
}

static void process_pending_sync_saves(void)
{
    uint32_t generation;
    while (sync_save_pending(&generation)) {
        esp_err_t err = save_to_nvs();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to synchronously persist schedules: %s",
                     esp_err_to_name(err));
        }
        complete_sync_save(generation, err);
    }
}

static void request_save(void)
{
    portENTER_CRITICAL(&s_lifecycle_lock);
    TaskHandle_t save_task = s_save_task;
    portEXIT_CRITICAL(&s_lifecycle_lock);
    if (save_task) xTaskNotify(save_task, SAVE_NOTIFY_ASYNC, eSetBits);
}

static void save_task_main(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_save_task_ready = true;
    xSemaphoreGive(s_mutex);

    const TickType_t debounce =
        pdMS_TO_TICKS(CONFIG_SCHEDULE_SAVE_DEBOUNCE_MS);
    while (true) {
        uint32_t notifications = 0;
        xTaskNotifyWait(0, UINT32_MAX, &notifications, portMAX_DELAY);
        if (notifications & SAVE_NOTIFY_SYNC) {
            process_pending_sync_saves();
        }
        if (!(notifications & SAVE_NOTIFY_ASYNC)) continue;

        while (true) {
            notifications = 0;
            if (xTaskNotifyWait(0, UINT32_MAX, &notifications, debounce) !=
                pdTRUE) {
                break;
            }
            if (notifications & SAVE_NOTIFY_SYNC) {
                process_pending_sync_saves();
            }
            // Coalesce a burst of schedule edits into one flash transaction.
        }

        // Notification bits can outlive the synchronous generation they refer
        // to and coalesce with a newer edit. Always snapshot after the debounce;
        // observing a SYNC bit alone does not prove that edit was persisted.
        esp_err_t err = save_to_nvs();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to persist schedules: %s",
                     esp_err_to_name(err));
        }
    }
}

static void process_schedule_end(int local_fan, int64_t now)
{
    int slot = -1;
    uint8_t restore_target = 0;
    bool inhibited = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    schedule_fan_state_t *fan = &s_fans[local_fan];
    if (!fan->operation_pending && fan->active_slot >= 0 &&
        (s_inhibited || now >= fan->active_end_us)) {
        fan->operation_pending = true;
        slot = fan->active_slot;
        restore_target = fan->saved_manual_target;
        inhibited = s_inhibited;
    }
    xSemaphoreGive(s_mutex);
    if (slot < 0) return;

    scheduler_target_request_t request = {
        .kind = SCHEDULER_TARGET_RESTORE,
        .local_fan = local_fan,
        .fan_number = s_config.app_config->fan_index_start + local_fan,
        .slot = slot,
        .target_pct = restore_target,
    };
    scheduler_target_result_t result;
    apply_target(&request, &result);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    fan->active_slot = -1;
    fan->active_end_us = 0;
    release_fan_operation_locked(local_fan);
    xSemaphoreGive(s_mutex);

    if (result.accepted) {
        ESP_LOGI(TAG, "Schedule ended for fan %d; restored %u%%",
                 request.fan_number, restore_target);
    }
    scheduler_event_t event = {
        .type = SCHEDULER_EVENT_ENDED,
        .local_fan = local_fan,
        .fan_number = request.fan_number,
        .slot = slot,
        .target_pct = restore_target,
        .inhibited = inhibited,
        .target_applied = result.accepted,
    };
    notify_event(&event);
}

static void process_schedule_start(int local_fan, int64_t now)
{
    int selected_slot = -1;
    uint8_t target_pct = 0;
    uint32_t duration_min = 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    schedule_fan_state_t *fan = &s_fans[local_fan];
    if (!fan->operation_pending && !s_inhibited && fan->active_slot < 0) {
        for (int candidate = 0; candidate < CONFIG_SCHEDULE_SLOTS; ++candidate) {
            int slot = (fan->cursor + candidate) % CONFIG_SCHEDULE_SLOTS;
            schedule_entry_t *entry = &fan->entries[slot];
            if (!entry->enabled) continue;
            if (entry->next_trigger_us == 0) {
                entry->next_trigger_us =
                    now + (int64_t)entry->interval_min * 60 * 1000000;
            }
            if (now < entry->next_trigger_us) continue;
            fan->operation_pending = true;
            selected_slot = slot;
            target_pct = entry->target_pct;
            duration_min = entry->duration_min;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    if (selected_slot < 0) return;

    scheduler_target_request_t request = {
        .kind = SCHEDULER_TARGET_START,
        .local_fan = local_fan,
        .fan_number = s_config.app_config->fan_index_start + local_fan,
        .slot = selected_slot,
        .target_pct = target_pct,
    };
    scheduler_target_result_t result;
    apply_target(&request, &result);

    bool started = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    fan = &s_fans[local_fan];
    schedule_entry_t *entry = &fan->entries[selected_slot];
    if (result.accepted && entry->enabled && !s_inhibited &&
        fan->active_slot < 0) {
        fan->saved_manual_target = result.manual_target_pct;
        fan->active_slot = selected_slot;
        fan->cursor = (uint8_t)((selected_slot + 1) % CONFIG_SCHEDULE_SLOTS);
        fan->active_end_us =
            now + (int64_t)duration_min * 60 * 1000000;
        entry->next_trigger_us =
            now + (int64_t)entry->interval_min * 60 * 1000000;
        started = true;
    }
    release_fan_operation_locked(local_fan);
    xSemaphoreGive(s_mutex);

    if (!started) return;
    ESP_LOGI(TAG,
             "Schedule slot %d started for fan %d: %u%% for %" PRIu32 " min",
             selected_slot, request.fan_number, target_pct, duration_min);
    scheduler_event_t event = {
        .type = SCHEDULER_EVENT_STARTED,
        .local_fan = local_fan,
        .fan_number = request.fan_number,
        .slot = selected_slot,
        .target_pct = target_pct,
        .inhibited = false,
        .target_applied = true,
    };
    notify_event(&event);
}

static void scheduler_task_main(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_scheduler_task_ready = true;
    xSemaphoreGive(s_mutex);

    while (true) {
        int64_t now = esp_timer_get_time();
        for (int fan = 0; fan < s_config.app_config->fan_count; ++fan) {
            process_schedule_end(fan, now);
            process_schedule_start(fan, now);
        }
        portENTER_CRITICAL(&s_heartbeat_lock);
        s_heartbeat_us = esp_timer_get_time();
        portEXIT_CRITICAL(&s_heartbeat_lock);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t scheduler_init(const scheduler_config_t *config)
{
    if (!config || !config->app_config || !config->apply_target ||
        config->app_config->fan_count < 1 ||
        config->app_config->fan_count > APP_CONFIG_MAX_FANS) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lifecycle_lock);
    if (s_initialized || s_init_started) {
        portEXIT_CRITICAL(&s_lifecycle_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_init_started = true;
    portEXIT_CRITICAL(&s_lifecycle_lock);

    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    SemaphoreHandle_t sync_mutex = xSemaphoreCreateMutex();
    SemaphoreHandle_t sync_complete = xSemaphoreCreateBinary();
    if (!mutex || !sync_mutex || !sync_complete) {
        if (mutex) vSemaphoreDelete(mutex);
        if (sync_mutex) vSemaphoreDelete(sync_mutex);
        if (sync_complete) vSemaphoreDelete(sync_complete);
        portENTER_CRITICAL(&s_lifecycle_lock);
        s_init_started = false;
        portEXIT_CRITICAL(&s_lifecycle_lock);
        return ESP_ERR_NO_MEM;
    }
    s_mutex = mutex;
    s_sync_mutex = sync_mutex;
    s_sync_complete = sync_complete;
    s_config = *config;
    for (int fan = 0; fan < APP_CONFIG_MAX_FANS; ++fan) {
        s_fans[fan].active_slot = -1;
    }

    // Invalid/missing persisted schedules leave the scheduler safely empty;
    // this matches the previous startup behavior, which logged and continued.
    (void)load_from_nvs();

    portENTER_CRITICAL(&s_lifecycle_lock);
    s_initialized = true;
    portEXIT_CRITICAL(&s_lifecycle_lock);
    return ESP_OK;
}

esp_err_t scheduler_start(void)
{
    portENTER_CRITICAL(&s_lifecycle_lock);
    if (!s_initialized || s_start_started) {
        portEXIT_CRITICAL(&s_lifecycle_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_start_started = true;
    portEXIT_CRITICAL(&s_lifecycle_lock);

    TaskHandle_t save_task = NULL;
    if (xTaskCreate(save_task_main, "schedule_nvs", SCHEDULER_SAVE_TASK_STACK,
                    NULL, SCHEDULER_SAVE_TASK_PRIORITY, &save_task) != pdPASS) {
        portENTER_CRITICAL(&s_lifecycle_lock);
        s_start_started = false;
        portEXIT_CRITICAL(&s_lifecycle_lock);
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_lifecycle_lock);
    s_save_task = save_task;
    portEXIT_CRITICAL(&s_lifecycle_lock);

    TaskHandle_t scheduler_task = NULL;
    if (xTaskCreatePinnedToCore(
            scheduler_task_main, "schedule", SCHEDULER_TASK_STACK, NULL,
            SCHEDULER_TASK_PRIORITY, &scheduler_task, 0) != pdPASS) {
        portENTER_CRITICAL(&s_lifecycle_lock);
        s_save_task = NULL;
        s_start_started = false;
        portEXIT_CRITICAL(&s_lifecycle_lock);
        vTaskDelete(save_task);
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_lifecycle_lock);
    s_scheduler_task = scheduler_task;
    portEXIT_CRITICAL(&s_lifecycle_lock);
    return ESP_OK;
}

esp_err_t scheduler_apply_manual_target(int local_fan, uint8_t target_pct,
                                        scheduler_target_result_t *out_result)
{
    if (!out_result || target_pct > 100 || !local_fan_valid(local_fan)) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_result = (scheduler_target_result_t) {0};
    if (!reserve_fan_operation(local_fan)) return ESP_ERR_TIMEOUT;

    scheduler_target_request_t request = {
        .kind = SCHEDULER_TARGET_MANUAL,
        .local_fan = local_fan,
        .fan_number = s_config.app_config->fan_index_start + local_fan,
        .slot = -1,
        .target_pct = target_pct,
    };
    scheduler_target_result_t result;
    apply_target(&request, &result);

    int cancelled_slot = -1;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (result.accepted && s_fans[local_fan].active_slot >= 0) {
        cancelled_slot = s_fans[local_fan].active_slot;
        s_fans[local_fan].active_slot = -1;
        s_fans[local_fan].active_end_us = 0;
    }
    release_fan_operation_locked(local_fan);
    xSemaphoreGive(s_mutex);
    *out_result = result;

    if (cancelled_slot >= 0) {
        scheduler_event_t event = {
            .type = SCHEDULER_EVENT_MANUAL_CANCELLED,
            .local_fan = local_fan,
            .fan_number = request.fan_number,
            .slot = cancelled_slot,
            .target_pct = target_pct,
            .target_applied = true,
        };
        notify_event(&event);
    }
    return ESP_OK;
}

esp_err_t scheduler_set_override(bool inhibited)
{
    if (!scheduler_is_initialized()) return ESP_ERR_INVALID_STATE;
    if (!inhibited) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        bool inhibit_latched = s_runtime_inhibit_latched;
        xSemaphoreGive(s_mutex);
        if (inhibit_latched) return ESP_ERR_INVALID_STATE;
    }
    if (!reserve_all_fans()) return ESP_ERR_TIMEOUT;

    int active_slots[APP_CONFIG_MAX_FANS];
    uint8_t restore_targets[APP_CONFIG_MAX_FANS];
    bool restore_applied[APP_CONFIG_MAX_FANS] = {false};
    for (int fan = 0; fan < APP_CONFIG_MAX_FANS; ++fan) {
        active_slots[fan] = -1;
        restore_targets[fan] = 0;
    }

    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    // A safety stop may latch after the early check but before all fan
    // reservations are acquired. Validate again at the state-change boundary.
    if (!inhibited && s_runtime_inhibit_latched) {
        release_all_fans_locked();
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_inhibited = inhibited;
    if (s_inhibited) {
        for (int fan = 0; fan < s_config.app_config->fan_count; ++fan) {
            active_slots[fan] = s_fans[fan].active_slot;
            restore_targets[fan] = s_fans[fan].saved_manual_target;
        }
    } else {
        for (int fan = 0; fan < s_config.app_config->fan_count; ++fan) {
            for (int slot = 0; slot < CONFIG_SCHEDULE_SLOTS; ++slot) {
                schedule_entry_t *entry = &s_fans[fan].entries[slot];
                if (entry->enabled) {
                    entry->next_trigger_us =
                        now + (int64_t)entry->interval_min * 60 * 1000000;
                }
            }
        }
    }
    xSemaphoreGive(s_mutex);

    /*
     * Keep every fan reserved across the restore callbacks. This makes the
     * ON transition a synchronous cancellation barrier: a concurrent OFF
     * transition cannot clear the inhibit until all active slots have been
     * restored and removed. Callbacks remain outside the scheduler mutex.
     */
    if (inhibited) {
        for (int fan = 0; fan < s_config.app_config->fan_count; ++fan) {
            if (active_slots[fan] < 0) continue;
            scheduler_target_request_t request = {
                .kind = SCHEDULER_TARGET_RESTORE,
                .local_fan = fan,
                .fan_number = s_config.app_config->fan_index_start + fan,
                .slot = active_slots[fan],
                .target_pct = restore_targets[fan],
            };
            scheduler_target_result_t result;
            apply_target(&request, &result);
            restore_applied[fan] = result.accepted;
        }
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (inhibited) {
        for (int fan = 0; fan < s_config.app_config->fan_count; ++fan) {
            if (active_slots[fan] < 0) continue;
            s_fans[fan].active_slot = -1;
            s_fans[fan].active_end_us = 0;
        }
    }
    release_all_fans_locked();
    xSemaphoreGive(s_mutex);
    request_save();

    ESP_LOGI(TAG, "Schedule override: %s", inhibited ? "ON" : "OFF");
    if (inhibited) {
        for (int fan = 0; fan < s_config.app_config->fan_count; ++fan) {
            if (active_slots[fan] < 0) continue;
            scheduler_event_t ended_event = {
                .type = SCHEDULER_EVENT_ENDED,
                .local_fan = fan,
                .fan_number = s_config.app_config->fan_index_start + fan,
                .slot = active_slots[fan],
                .target_pct = restore_targets[fan],
                .inhibited = true,
                .target_applied = restore_applied[fan],
            };
            notify_event(&ended_event);
        }
    }
    scheduler_event_t event = {
        .type = SCHEDULER_EVENT_OVERRIDE_CHANGED,
        .local_fan = -1,
        .slot = -1,
        .inhibited = inhibited,
    };
    notify_event(&event);
    return ESP_OK;
}

esp_err_t scheduler_get_override(bool *out_inhibited)
{
    if (!out_inhibited) return ESP_ERR_INVALID_ARG;
    if (!scheduler_is_initialized()) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out_inhibited = s_inhibited;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t scheduler_set_entry(int local_fan, int slot,
                              uint32_t interval_min, uint32_t duration_min,
                              uint8_t target_pct)
{
    if (!local_fan_valid(local_fan) || !slot_valid(slot) ||
        !entry_config_valid(interval_min, duration_min, target_pct)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!reserve_fan_operation(local_fan)) return ESP_ERR_TIMEOUT;

    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    schedule_entry_t *entry = &s_fans[local_fan].entries[slot];
    entry->enabled = true;
    entry->interval_min = interval_min;
    entry->duration_min = duration_min;
    entry->target_pct = target_pct;
    entry->next_trigger_us =
        now + (int64_t)interval_min * 60 * 1000000;
    release_fan_operation_locked(local_fan);
    xSemaphoreGive(s_mutex);
    request_save();

    ESP_LOGI(TAG,
             "Schedule %d/%d set: every %" PRIu32 "min for %" PRIu32
             "min at %u%%",
             s_config.app_config->fan_index_start + local_fan, slot,
             interval_min, duration_min, target_pct);
    scheduler_event_t event = {
        .type = SCHEDULER_EVENT_ENTRY_SET,
        .local_fan = local_fan,
        .fan_number = s_config.app_config->fan_index_start + local_fan,
        .slot = slot,
        .target_pct = target_pct,
    };
    notify_event(&event);
    return ESP_OK;
}

esp_err_t scheduler_disable_entry(int local_fan, int slot)
{
    if (!local_fan_valid(local_fan) || !slot_valid(slot)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!reserve_fan_operation(local_fan)) return ESP_ERR_TIMEOUT;

    bool was_active;
    uint8_t restore_target;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    was_active = s_fans[local_fan].active_slot == slot;
    restore_target = s_fans[local_fan].saved_manual_target;
    xSemaphoreGive(s_mutex);

    scheduler_target_result_t result = {0};
    if (was_active) {
        scheduler_target_request_t request = {
            .kind = SCHEDULER_TARGET_RESTORE,
            .local_fan = local_fan,
            .fan_number = s_config.app_config->fan_index_start + local_fan,
            .slot = slot,
            .target_pct = restore_target,
        };
        apply_target(&request, &result);
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (was_active) {
        s_fans[local_fan].active_slot = -1;
        s_fans[local_fan].active_end_us = 0;
    }
    schedule_entry_t *entry = &s_fans[local_fan].entries[slot];
    entry->enabled = false;
    entry->next_trigger_us = 0;
    release_fan_operation_locked(local_fan);
    xSemaphoreGive(s_mutex);
    request_save();

    scheduler_event_t event = {
        .type = SCHEDULER_EVENT_ENTRY_DISABLED,
        .local_fan = local_fan,
        .fan_number = s_config.app_config->fan_index_start + local_fan,
        .slot = slot,
        .target_pct = restore_target,
        .target_applied = !was_active || result.accepted,
    };
    notify_event(&event);
    return ESP_OK;
}

esp_err_t scheduler_get_entry(int local_fan, int slot,
                              scheduler_entry_snapshot_t *out_snapshot)
{
    if (!out_snapshot || !local_fan_valid(local_fan) || !slot_valid(slot)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const schedule_fan_state_t *fan = &s_fans[local_fan];
    const schedule_entry_t *entry = &fan->entries[slot];
    *out_snapshot = (scheduler_entry_snapshot_t) {
        .enabled = entry->enabled,
        .interval_min = entry->interval_min,
        .duration_min = entry->duration_min,
        .target_pct = entry->target_pct,
        .next_trigger_us = entry->next_trigger_us,
        .active = fan->active_slot == slot,
        .active_end_us = fan->active_slot == slot ? fan->active_end_us : 0,
    };
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t scheduler_cancel_fan(int local_fan)
{
    if (!local_fan_valid(local_fan)) return ESP_ERR_INVALID_ARG;
    if (!reserve_fan_operation(local_fan)) return ESP_ERR_TIMEOUT;

    int cancelled_slot;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    cancelled_slot = s_fans[local_fan].active_slot;
    s_fans[local_fan].active_slot = -1;
    s_fans[local_fan].active_end_us = 0;
    release_fan_operation_locked(local_fan);
    xSemaphoreGive(s_mutex);

    if (cancelled_slot >= 0) {
        scheduler_event_t event = {
            .type = SCHEDULER_EVENT_FAN_CANCELLED,
            .local_fan = local_fan,
            .fan_number = s_config.app_config->fan_index_start + local_fan,
            .slot = cancelled_slot,
        };
        notify_event(&event);
    }
    return ESP_OK;
}

esp_err_t scheduler_try_inhibit_all(TickType_t timeout_ticks)
{
    if (!scheduler_is_initialized()) return ESP_ERR_INVALID_STATE;

    TickType_t started = xTaskGetTickCount();
    while (true) {
        TickType_t remaining = portMAX_DELAY;
        if (timeout_ticks != portMAX_DELAY) {
            TickType_t elapsed = xTaskGetTickCount() - started;
            remaining = elapsed >= timeout_ticks ? 0 : timeout_ticks - elapsed;
        }
        if (xSemaphoreTake(s_mutex, remaining) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }

        bool operation_pending = false;
        for (int fan = 0; fan < s_config.app_config->fan_count; ++fan) {
            if (s_fans[fan].operation_pending) {
                operation_pending = true;
                break;
            }
        }
        if (!operation_pending) {
            s_inhibited = true;
            s_runtime_inhibit_latched = true;
            for (int fan = 0; fan < s_config.app_config->fan_count; ++fan) {
                s_fans[fan].active_slot = -1;
                s_fans[fan].active_end_us = 0;
            }
            xSemaphoreGive(s_mutex);
            break;
        }
        xSemaphoreGive(s_mutex);

        if (timeout_ticks != portMAX_DELAY &&
            xTaskGetTickCount() - started >= timeout_ticks) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
    TickType_t remaining = portMAX_DELAY;
    if (timeout_ticks != portMAX_DELAY) {
        TickType_t elapsed = xTaskGetTickCount() - started;
        remaining = elapsed >= timeout_ticks ? 0 : timeout_ticks - elapsed;
    }
    esp_err_t persist_error = scheduler_save_sync(remaining);
    if (persist_error != ESP_OK) {
        // Preserve the previous best-effort behavior even when the bounded
        // acknowledgement window expires or the writer is not yet running.
        request_save();
    }

    scheduler_event_t event = {
        .type = SCHEDULER_EVENT_INHIBITED_ALL,
        .local_fan = -1,
        .slot = -1,
        .inhibited = true,
    };
    notify_event(&event);
    return persist_error;
}

esp_err_t scheduler_inhibit_all(void)
{
    return scheduler_try_inhibit_all(portMAX_DELAY);
}

esp_err_t scheduler_save_sync(TickType_t timeout_ticks)
{
    const TickType_t started_at = xTaskGetTickCount();
    if (!scheduler_is_initialized() || !s_sync_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_sync_mutex, timeout_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t result = ESP_ERR_INVALID_STATE;
    TaskHandle_t save_task_handle;
    uint32_t generation = 0;
    portENTER_CRITICAL(&s_lifecycle_lock);
    save_task_handle = s_save_task;
    if (s_initialized && save_task_handle &&
        save_task_handle != xTaskGetCurrentTaskHandle()) {
        generation = ++s_sync_requested_generation;
        result = ESP_OK;
    }
    portEXIT_CRITICAL(&s_lifecycle_lock);

    if (result != ESP_OK) {
        xSemaphoreGive(s_sync_mutex);
        return result;
    }

    xTaskNotify(save_task_handle, SAVE_NOTIFY_SYNC, eSetBits);
    while (true) {
        portENTER_CRITICAL(&s_lifecycle_lock);
        bool completed = s_sync_completed_generation == generation;
        if (completed) result = s_sync_result;
        portEXIT_CRITICAL(&s_lifecycle_lock);
        if (completed) break;

        TickType_t remaining = portMAX_DELAY;
        if (timeout_ticks != portMAX_DELAY) {
            TickType_t elapsed = xTaskGetTickCount() - started_at;
            if (elapsed >= timeout_ticks) {
                result = ESP_ERR_TIMEOUT;
                break;
            }
            remaining = timeout_ticks - elapsed;
        }
        if (xSemaphoreTake(s_sync_complete, remaining) != pdTRUE) {
            result = ESP_ERR_TIMEOUT;
            break;
        }
    }

    xSemaphoreGive(s_sync_mutex);
    return result;
}

scheduler_health_snapshot_t scheduler_health_snapshot(void)
{
    scheduler_health_snapshot_t snapshot = {
        .feature_enabled = true,
    };
    portENTER_CRITICAL(&s_lifecycle_lock);
    bool initialized = s_initialized;
    SemaphoreHandle_t mutex = s_mutex;
    TaskHandle_t scheduler_task = s_scheduler_task;
    TaskHandle_t save_task = s_save_task;
    portEXIT_CRITICAL(&s_lifecycle_lock);
    snapshot.initialized = initialized;
    if (!mutex) return snapshot;

    xSemaphoreTake(mutex, portMAX_DELAY);
    snapshot.scheduler_task_ready = s_scheduler_task_ready;
    snapshot.save_task_ready = s_save_task_ready;
    snapshot.inhibited = s_inhibited;
    snapshot.heartbeat_us = scheduler_get_heartbeat_us();
    xSemaphoreGive(mutex);
    if (scheduler_task) {
        snapshot.scheduler_task_stack_bytes =
            uxTaskGetStackHighWaterMark(scheduler_task);
    }
    if (save_task) {
        snapshot.save_task_stack_bytes = uxTaskGetStackHighWaterMark(save_task);
    }
    return snapshot;
}

int64_t scheduler_get_heartbeat_us(void)
{
    portENTER_CRITICAL(&s_heartbeat_lock);
    int64_t heartbeat = s_heartbeat_us;
    portEXIT_CRITICAL(&s_heartbeat_lock);
    return heartbeat;
}

#else

static bool s_stub_initialized;
static scheduler_config_t s_stub_config;

esp_err_t scheduler_init(const scheduler_config_t *config)
{
    if (!config || !config->app_config || !config->apply_target) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_stub_initialized) return ESP_ERR_INVALID_STATE;
    s_stub_config = *config;
    s_stub_initialized = true;
    return ESP_OK;
}

esp_err_t scheduler_start(void)
{
    return s_stub_initialized ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t scheduler_apply_manual_target(int local_fan, uint8_t target_pct,
                                        scheduler_target_result_t *out_result)
{
    if (!out_result || !s_stub_initialized || local_fan < 0 ||
        local_fan >= s_stub_config.app_config->fan_count || target_pct > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_result = (scheduler_target_result_t) {0};
    scheduler_target_request_t request = {
        .kind = SCHEDULER_TARGET_MANUAL,
        .local_fan = local_fan,
        .fan_number = s_stub_config.app_config->fan_index_start + local_fan,
        .slot = -1,
        .target_pct = target_pct,
    };
    s_stub_config.apply_target(&request, out_result,
                               s_stub_config.callback_context);
    if (out_result->manual_target_pct > 100) out_result->accepted = false;
    return ESP_OK;
}

esp_err_t scheduler_set_override(bool inhibited)
{
    (void)inhibited;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t scheduler_get_override(bool *out_inhibited)
{
    if (!out_inhibited) return ESP_ERR_INVALID_ARG;
    *out_inhibited = false;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t scheduler_set_entry(int local_fan, int slot,
                              uint32_t interval_min, uint32_t duration_min,
                              uint8_t target_pct)
{
    (void)local_fan;
    (void)slot;
    (void)interval_min;
    (void)duration_min;
    (void)target_pct;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t scheduler_disable_entry(int local_fan, int slot)
{
    (void)local_fan;
    (void)slot;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t scheduler_get_entry(int local_fan, int slot,
                              scheduler_entry_snapshot_t *out_snapshot)
{
    (void)local_fan;
    (void)slot;
    if (!out_snapshot) return ESP_ERR_INVALID_ARG;
    *out_snapshot = (scheduler_entry_snapshot_t) {0};
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t scheduler_cancel_fan(int local_fan)
{
    (void)local_fan;
    return ESP_OK;
}

esp_err_t scheduler_inhibit_all(void)
{
    return ESP_OK;
}

esp_err_t scheduler_try_inhibit_all(TickType_t timeout_ticks)
{
    (void)timeout_ticks;
    return ESP_OK;
}

esp_err_t scheduler_save_sync(TickType_t timeout_ticks)
{
    (void)timeout_ticks;
    return ESP_OK;
}

scheduler_health_snapshot_t scheduler_health_snapshot(void)
{
    return (scheduler_health_snapshot_t) {
        .feature_enabled = false,
        .initialized = s_stub_initialized,
        .inhibited = false,
    };
}

int64_t scheduler_get_heartbeat_us(void)
{
    return 0;
}

#endif
