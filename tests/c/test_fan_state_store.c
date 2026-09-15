/* Real persistence/restore implementation with a transactional fake NVS. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#define CONFIG_RESTORE_FAN_STATE 1
#define CONFIG_FAN_STATE_SAVE_DEBOUNCE_MS 100
#include "../../main/fan_state_store.c"

struct scheduler_test_semaphore { bool available; };
static fan_state_blob_t committed, pending;
static bool present, snapshot_ok = true;
static esp_err_t commit_error, write_error;
static uint8_t target = 50, restored;
static unsigned restores;
static app_config_t config = {.fan_count = 1, .fan_index_start = 1,
    .topology_fingerprint = 1234};

void scheduler_test_enter_critical(portMUX_TYPE *lock)
{ assert(!lock->locked); lock->locked = 1; }
void scheduler_test_exit_critical(portMUX_TYPE *lock)
{ assert(lock->locked); lock->locked = 0; }
void scheduler_test_log(const char *tag, const char *format, ...)
{ (void)tag; (void)format; }
const char *esp_err_to_name(esp_err_t err) { (void)err; return "error"; }
SemaphoreHandle_t xSemaphoreCreateBinary(void)
{ return calloc(1, sizeof(struct scheduler_test_semaphore)); }
SemaphoreHandle_t xSemaphoreCreateMutex(void)
{ SemaphoreHandle_t mutex = xSemaphoreCreateBinary(); mutex->available = true; return mutex; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t ticks)
{ (void)ticks; if (!mutex->available) return pdFALSE; mutex->available = false; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex) { mutex->available = true; return pdTRUE; }
void vSemaphoreDelete(SemaphoreHandle_t mutex) { free(mutex); }
TickType_t xTaskGetTickCount(void) { return 0; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return NULL; }
BaseType_t xTaskNotify(TaskHandle_t task, uint32_t bits, eNotifyAction action)
{
    (void)task; assert(action == eSetBits);
    if (bits & SAVE_NOTIFY_SYNC) process_pending_sync_saves();
    return pdTRUE;
}
BaseType_t xTaskNotifyWait(uint32_t entry, uint32_t exit_bits, uint32_t *bits, TickType_t ticks)
{ (void)entry; (void)exit_bits; (void)bits; (void)ticks; assert(false); return pdFALSE; }
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack,
                       void *arg, UBaseType_t priority, TaskHandle_t *task)
{ (void)fn; (void)name; (void)stack; (void)arg; (void)priority; *task = &target; return pdPASS; }
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task) { (void)task; return 1024; }
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{ (void)mode; *handle = strcmp(name, FAN_STATE_NAMESPACE) == 0 ? 1 : 2; return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *length)
{
    assert(handle == 1);
    if (!present || strcmp(key, FAN_STATE_NVS_KEY) != 0) return ESP_ERR_NVS_NOT_FOUND;
    assert(*length >= sizeof(committed)); *length = sizeof(committed);
    memcpy(value, &committed, sizeof(committed)); return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    assert(handle == 1 && strcmp(key, FAN_STATE_NVS_KEY) == 0 && length == sizeof(pending));
    if (write_error != ESP_OK) return write_error;
    memcpy(&pending, value, length); return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    if (commit_error != ESP_OK) return commit_error;
    if (handle == 1) { committed = pending; present = true; }
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { (void)handle; }
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{ (void)handle; (void)key; (void)value; return ESP_OK; }
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{ (void)handle; (void)key; *value = 0; return ESP_ERR_NVS_NOT_FOUND; }
static bool snapshot(void *context, uint8_t *targets, size_t count)
{ (void)context; assert(count == 1); targets[0] = target; return snapshot_ok; }
static bool restore(void *context, const uint8_t *targets, size_t count)
{ (void)context; assert(count == 1); restored = targets[0]; ++restores; return true; }

int main(void)
{
    fan_state_store_config_t cfg = {.app_config = &config,
        .snapshot_targets = snapshot, .restore_targets = restore};
    assert(fan_state_store_init(&cfg, false) == ESP_OK);
    assert(fan_state_store_start() == ESP_OK);
    assert(fan_state_store_save_sync(100) == ESP_OK);
    fan_state_store_restore(); assert(restores == 1 && restored == 50);

    /* A failed zero-target commit must be reported, and cannot replace the
     * previously committed record. Callers can then keep the output inhibited. */
    target = 0; commit_error = ESP_FAIL;
    assert(fan_state_store_save_sync(100) == ESP_FAIL);
    fan_state_store_restore(); assert(restores == 2 && restored == 50);
    commit_error = ESP_OK;
    assert(fan_state_store_save_sync(100) == ESP_OK);
    fan_state_store_restore(); assert(restores == 3 && restored == 0);

    write_error = ESP_FAIL;
    assert(fan_state_store_save_sync(100) == ESP_FAIL);
    write_error = ESP_OK; snapshot_ok = false;
    assert(fan_state_store_save_sync(100) == ESP_ERR_INVALID_STATE);
    snapshot_ok = true;

    committed.targets[0] ^= 1; /* Simulated torn/corrupt record. */
    fan_state_store_restore(); assert(restores == 3);
    assert(fan_state_store_save_sync(100) == ESP_OK);
    config.topology_fingerprint++;
    fan_state_store_restore(); assert(restores == 3);
    vSemaphoreDelete(s_sync_mutex); vSemaphoreDelete(s_sync_complete);
    puts("fan_state_store: commit failure, retry, write failure, snapshot failure, corruption, topology passed");
    return 0;
}
