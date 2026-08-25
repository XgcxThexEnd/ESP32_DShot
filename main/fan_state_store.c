#include "fan_state_store.h"

#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#define FAN_STATE_NAMESPACE        "fan_state"
#define BOOT_HEALTH_NAMESPACE      "boot_health"
#define FAN_STATE_NVS_KEY          "targets_v3"
#define FAN_STATE_MAGIC            0x46535433U
#define FAN_STATE_SCHEMA_VERSION   3U
#define FAN_STATE_V2_NVS_KEY       "targets_v2"
#define FAN_STATE_V2_MAGIC         0x46535432U
#define FAN_STATE_V2_VERSION       2U
#define FAN_STATE_LEGACY_NVS_KEY   "targets"

#ifdef CONFIG_RESTORE_FAN_STATE
#define SAVE_NOTIFY_ASYNC          (1UL << 0)
#define SAVE_NOTIFY_SYNC           (1UL << 1)
#endif

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t length;
    uint64_t topology_fingerprint;
    int32_t fan_index_start;
    uint8_t fan_count;
    uint8_t targets[APP_CONFIG_MAX_FANS];
    uint8_t reserved[3];
    uint32_t checksum;
} fan_state_blob_t;

/** Exact schema-v2 layout, retained only for safe all-zero migration. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t length;
    int32_t fan_index_start;
    uint8_t fan_count;
    uint8_t targets[APP_CONFIG_MAX_FANS];
    uint8_t reserved[3];
    uint32_t checksum;
} fan_state_blob_v2_t;

_Static_assert(sizeof(fan_state_blob_t) == 32,
               "fan-state schema-v3 layout changed");
_Static_assert(sizeof(fan_state_blob_v2_t) == 24,
               "fan-state schema-v2 migration layout changed");
_Static_assert(offsetof(fan_state_blob_v2_t, checksum) == 20,
               "fan-state schema-v2 checksum offset changed");

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static fan_state_store_config_t s_config;
static TaskHandle_t s_save_task;
static bool s_initialized;
static bool s_save_task_ready;
static bool s_erased_on_boot;
static bool s_erase_observed;

#ifdef CONFIG_RESTORE_FAN_STATE
static const char *TAG = "fan_state_store";
static SemaphoreHandle_t s_sync_mutex;
static SemaphoreHandle_t s_sync_complete;
static uint32_t s_sync_requested_generation;
static uint32_t s_sync_completed_generation;
static esp_err_t s_sync_result = ESP_ERR_INVALID_STATE;

static uint32_t checksum(const void *data, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < length; ++i) {
        hash = (hash ^ bytes[i]) * 16777619U;
    }
    return hash;
}
#endif

static esp_err_t update_erase_observation(bool erased_now)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(BOOT_HEALTH_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    if (erased_now) {
        err = nvs_set_u8(handle, "erase_seen", 1);
        if (err == ESP_OK) err = nvs_commit(handle);
    }
    uint8_t seen = 0;
    if (err == ESP_OK) {
        esp_err_t read_err = nvs_get_u8(handle, "erase_seen", &seen);
        if (read_err != ESP_OK && read_err != ESP_ERR_NVS_NOT_FOUND) err = read_err;
    }
    nvs_close(handle);
    portENTER_CRITICAL(&s_lock);
    s_erase_observed = erased_now || seen == 1;
    portEXIT_CRITICAL(&s_lock);
    return err;
}

#ifdef CONFIG_RESTORE_FAN_STATE
static esp_err_t save_now(void)
{
    fan_state_blob_t state = {
        .magic = FAN_STATE_MAGIC,
        .version = FAN_STATE_SCHEMA_VERSION,
        .length = sizeof(fan_state_blob_t),
        .topology_fingerprint = s_config.app_config->topology_fingerprint,
        .fan_index_start = s_config.app_config->fan_index_start,
        .fan_count = s_config.app_config->fan_count,
    };
    if (!s_config.snapshot_targets(
            s_config.callback_context, state.targets,
            (size_t)s_config.app_config->fan_count)) {
        return ESP_ERR_INVALID_STATE;
    }
    state.checksum = checksum(&state, offsetof(fan_state_blob_t, checksum));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(FAN_STATE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, FAN_STATE_NVS_KEY, &state, sizeof(state));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static bool sync_save_pending(uint32_t *generation)
{
    bool pending;
    portENTER_CRITICAL(&s_lock);
    pending = s_sync_requested_generation != s_sync_completed_generation;
    if (pending) *generation = s_sync_requested_generation;
    portEXIT_CRITICAL(&s_lock);
    return pending;
}

static void complete_sync_save(uint32_t generation, esp_err_t result)
{
    portENTER_CRITICAL(&s_lock);
    s_sync_completed_generation = generation;
    s_sync_result = result;
    portEXIT_CRITICAL(&s_lock);
    xSemaphoreGive(s_sync_complete);
}

static void process_pending_sync_saves(void)
{
    uint32_t generation;
    while (sync_save_pending(&generation)) {
        esp_err_t err = save_now();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to synchronously persist fan states: %s",
                     esp_err_to_name(err));
        }
        complete_sync_save(generation, err);
    }
}

static void save_task(void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&s_lock);
    s_save_task_ready = true;
    portEXIT_CRITICAL(&s_lock);
    const TickType_t debounce =
        pdMS_TO_TICKS(CONFIG_FAN_STATE_SAVE_DEBOUNCE_MS);
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
            // Every asynchronous request restarts the quiet-period debounce.
        }

        // A synchronous request may have arrived at the debounce boundary.
        // Service it first; a successful synchronous snapshot also satisfies
        // all asynchronous requests received before that snapshot.
        uint32_t generation;
        if (sync_save_pending(&generation)) {
            process_pending_sync_saves();
            continue;
        }

        esp_err_t err = save_now();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to persist fan states: %s",
                     esp_err_to_name(err));
        }
    }
}
#endif

esp_err_t fan_state_store_init(const fan_state_store_config_t *config,
                               bool erased_on_boot)
{
    if (!config || !config->app_config || !config->snapshot_targets ||
        !config->restore_targets) {
        return ESP_ERR_INVALID_ARG;
    }
#ifdef CONFIG_RESTORE_FAN_STATE
    SemaphoreHandle_t sync_mutex = xSemaphoreCreateMutex();
    SemaphoreHandle_t sync_complete = xSemaphoreCreateBinary();
    if (!sync_mutex || !sync_complete) {
        if (sync_mutex) vSemaphoreDelete(sync_mutex);
        if (sync_complete) vSemaphoreDelete(sync_complete);
        return ESP_ERR_NO_MEM;
    }
#endif
    portENTER_CRITICAL(&s_lock);
    if (s_initialized) {
        portEXIT_CRITICAL(&s_lock);
#ifdef CONFIG_RESTORE_FAN_STATE
        vSemaphoreDelete(sync_mutex);
        vSemaphoreDelete(sync_complete);
#endif
        return ESP_ERR_INVALID_STATE;
    }
    s_config = *config;
    s_erased_on_boot = erased_on_boot;
#ifdef CONFIG_RESTORE_FAN_STATE
    s_sync_mutex = sync_mutex;
    s_sync_complete = sync_complete;
#endif
    s_initialized = true;
    portEXIT_CRITICAL(&s_lock);
    return update_erase_observation(erased_on_boot);
}

esp_err_t fan_state_store_start(void)
{
    portENTER_CRITICAL(&s_lock);
    bool initialized = s_initialized;
    portEXIT_CRITICAL(&s_lock);
    if (!initialized) return ESP_ERR_INVALID_STATE;
#ifdef CONFIG_RESTORE_FAN_STATE
    if (s_save_task) return ESP_OK;
    if (xTaskCreate(save_task, "fan_nvs", 3072, NULL, 4, &s_save_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
#endif
    return ESP_OK;
}

void fan_state_store_request_save(void)
{
#ifdef CONFIG_RESTORE_FAN_STATE
    if (s_save_task) {
        xTaskNotify(s_save_task, SAVE_NOTIFY_ASYNC, eSetBits);
    }
#endif
}

esp_err_t fan_state_store_save_sync(TickType_t timeout_ticks)
{
#ifndef CONFIG_RESTORE_FAN_STATE
    (void)timeout_ticks;
    return ESP_OK;
#else
    const TickType_t started_at = xTaskGetTickCount();
    if (!s_sync_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_sync_mutex, timeout_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t result = ESP_ERR_INVALID_STATE;
    TaskHandle_t save_task_handle;
    uint32_t generation;
    portENTER_CRITICAL(&s_lock);
    save_task_handle = s_save_task;
    if (s_initialized && save_task_handle &&
        save_task_handle != xTaskGetCurrentTaskHandle()) {
        generation = ++s_sync_requested_generation;
        result = ESP_OK;
    }
    portEXIT_CRITICAL(&s_lock);

    if (result != ESP_OK) {
        xSemaphoreGive(s_sync_mutex);
        return result;
    }

    xTaskNotify(save_task_handle, SAVE_NOTIFY_SYNC, eSetBits);
    while (true) {
        portENTER_CRITICAL(&s_lock);
        bool completed = s_sync_completed_generation == generation;
        if (completed) result = s_sync_result;
        portEXIT_CRITICAL(&s_lock);
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
#endif
}

void fan_state_store_restore(void)
{
#ifdef CONFIG_RESTORE_FAN_STATE
    if (!s_initialized) return;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(FAN_STATE_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open persisted fan states: %s",
                 esp_err_to_name(err));
        return;
    }

    fan_state_blob_t state = {0};
    size_t length = sizeof(state);
    err = nvs_get_blob(handle, FAN_STATE_NVS_KEY, &state, &length);
    bool valid = err == ESP_OK && length == sizeof(state) &&
                 state.magic == FAN_STATE_MAGIC &&
                 state.version == FAN_STATE_SCHEMA_VERSION &&
                 state.length == sizeof(state) &&
                 state.topology_fingerprint ==
                     s_config.app_config->topology_fingerprint &&
                 state.fan_index_start == s_config.app_config->fan_index_start &&
                 state.fan_count == s_config.app_config->fan_count &&
                 state.checksum ==
                     checksum(&state, offsetof(fan_state_blob_t, checksum));
    for (int i = 0; valid && i < APP_CONFIG_MAX_FANS; ++i) {
        if (state.targets[i] > 100) valid = false;
    }

    if (err == ESP_OK && valid) {
        if (s_config.restore_targets(s_config.callback_context, state.targets,
                                     (size_t)s_config.app_config->fan_count)) {
            ESP_LOGI(TAG, "Restored fan states after zero-throttle arming period");
        } else {
            ESP_LOGW(TAG,
                     "Skipped fan-state restore because a safety latch is active");
        }
        nvs_close(handle);
        return;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Rejected invalid or incompatible fan-state blob");
        nvs_close(handle);
        return;
    }

    /*
     * Schema v2 predates the hardware-topology fingerprint. It is therefore
     * never safe to translate a nonzero target. A checksum-valid blob whose
     * entire fixed target array is zero is topology-independent and may seed
     * the new schema without authorizing motion.
     */
    fan_state_blob_v2_t v2 = {0};
    length = sizeof(v2);
    err = nvs_get_blob(handle, FAN_STATE_V2_NVS_KEY, &v2, &length);
    bool v2_valid = err == ESP_OK && length == sizeof(v2) &&
                    v2.magic == FAN_STATE_V2_MAGIC &&
                    v2.version == FAN_STATE_V2_VERSION &&
                    v2.length == sizeof(v2) && v2.fan_count >= 1 &&
                    v2.fan_count <= APP_CONFIG_MAX_FANS &&
                    v2.checksum ==
                        checksum(&v2, offsetof(fan_state_blob_v2_t, checksum));
    for (int i = 0; v2_valid && i < APP_CONFIG_MAX_FANS; ++i) {
        if (v2.targets[i] != 0) v2_valid = false;
    }
    if (err == ESP_OK) {
        if (v2_valid && s_config.restore_targets(
                            s_config.callback_context, v2.targets,
                            (size_t)s_config.app_config->fan_count)) {
            fan_state_store_request_save();
            ESP_LOGI(TAG, "Migrated all-zero schema-v2 fan state");
        } else {
            ESP_LOGW(TAG,
                     "Ignored schema-v2 fan state; only valid all-zero data is migratable");
        }
        nvs_close(handle);
        return;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Failed to inspect schema-v2 fan state: %s",
                 esp_err_to_name(err));
        nvs_close(handle);
        return;
    }

    uint8_t legacy[8] = {0};
    length = sizeof(legacy);
    err = nvs_get_blob(handle, FAN_STATE_LEGACY_NVS_KEY, legacy, &length);
    bool legacy_valid = err == ESP_OK && length == sizeof(legacy);
    for (size_t i = 0; legacy_valid && i < sizeof(legacy); ++i) {
        if (legacy[i] != 0) legacy_valid = false;
    }
    if (err == ESP_OK) {
        if (legacy_valid && s_config.restore_targets(
                                s_config.callback_context, legacy,
                                (size_t)s_config.app_config->fan_count)) {
            fan_state_store_request_save();
            ESP_LOGI(TAG, "Migrated all-zero legacy fan state");
        } else {
            ESP_LOGW(TAG,
                     "Ignored legacy fan state; only all-zero data is migratable");
        }
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Failed to inspect legacy fan state: %s",
                 esp_err_to_name(err));
    }
    nvs_close(handle);
#endif
}

fan_state_store_health_t fan_state_store_health_snapshot(void)
{
    fan_state_store_health_t snapshot;
    TaskHandle_t task;
    portENTER_CRITICAL(&s_lock);
    snapshot = (fan_state_store_health_t) {
        .initialized = s_initialized,
        .save_task_ready = s_save_task_ready,
        .erased_on_boot = s_erased_on_boot,
        .erase_observed = s_erase_observed,
        .save_task_stack_words = 0,
    };
    task = s_save_task;
    portEXIT_CRITICAL(&s_lock);
    if (task) snapshot.save_task_stack_words = uxTaskGetStackHighWaterMark(task);
    return snapshot;
}
