#include "ota_manager.h"

#include "sdkconfig.h"

#ifdef CONFIG_OTA_ENABLED

#if !defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)
#error "CONFIG_OTA_ENABLED requires bootloader application rollback"
#endif

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "controller_policy.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "ota_download.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define OTA_MANAGER_TASK_STACK_SIZE 8192
#define OTA_MANAGER_TASK_PRIORITY 5
#define OTA_MANAGER_TASK_CORE 0
#define OTA_MANAGER_REBOOT_STATUS_GRACE_MS 1000

static const char *TAG = "ota_manager";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static ota_manager_config_t s_config;
static bool s_initialized;
static bool s_in_progress;
static TaskHandle_t s_task;
static char *s_url_copy;

static void ota_manager_task(void *arg);

static void publish_status(const char *status, bool retain)
{
    ota_manager_publish_status_fn callback = NULL;
    void *context = NULL;

    portENTER_CRITICAL(&s_lock);
    if (s_initialized) {
        callback = s_config.publish_status;
        context = s_config.callback_context;
    }
    portEXIT_CRITICAL(&s_lock);

    if (callback) callback(context, status, retain);
}

static void abort_prepared_update(void)
{
    ota_manager_abort_fn callback = NULL;
    void *context = NULL;

    portENTER_CRITICAL(&s_lock);
    if (s_initialized) {
        callback = s_config.abort;
        context = s_config.callback_context;
    }
    portEXIT_CRITICAL(&s_lock);

    if (callback) callback(context);
}

static void release_claim(char *owned_url)
{
    portENTER_CRITICAL(&s_lock);
    if (!owned_url || s_url_copy == owned_url) {
        s_url_copy = NULL;
        s_task = NULL;
        s_in_progress = false;
    }
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t ota_manager_init(const ota_manager_config_t *config)
{
    if (!config || !config->prepare || !config->trusted_time || !config->abort ||
        !config->publish_status) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    if (s_initialized || s_in_progress) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_config = *config;
    s_initialized = true;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

bool ota_manager_url_allowed(const char *url)
{
    return controller_https_url_allowed(url, CONFIG_OTA_ALLOWED_URL_PREFIX);
}

bool ota_manager_is_in_progress(void)
{
    portENTER_CRITICAL(&s_lock);
    bool active = s_in_progress;
    portEXIT_CRITICAL(&s_lock);
    return active;
}

ota_manager_start_result_t ota_manager_start(const char *url)
{
    if (!url) return OTA_MANAGER_START_INVALID_ARGUMENT;

    ota_manager_trusted_time_fn trusted_time = NULL;
    void *context = NULL;
    portENTER_CRITICAL(&s_lock);
    if (s_initialized) {
        trusted_time = s_config.trusted_time;
        context = s_config.callback_context;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!trusted_time) return OTA_MANAGER_START_NOT_INITIALIZED;

    if (!ota_manager_url_allowed(url)) {
        ESP_LOGW(TAG, "Rejected OTA URL outside the configured HTTPS prefix");
        return OTA_MANAGER_START_REJECTED_URL;
    }
    if (!trusted_time(context)) {
        ESP_LOGW(TAG, "Rejected OTA request until TLS wall time is trusted");
        return OTA_MANAGER_START_TIME_NOT_READY;
    }

    size_t url_length = strlen(url);
    if (url_length == SIZE_MAX) return OTA_MANAGER_START_ALLOC_FAILED;
    char *url_copy = malloc(url_length + 1U);
    if (!url_copy) {
        ESP_LOGE(TAG, "OTA URL allocation failed");
        return OTA_MANAGER_START_ALLOC_FAILED;
    }
    memcpy(url_copy, url, url_length + 1U);

    portENTER_CRITICAL(&s_lock);
    if (s_in_progress) {
        portEXIT_CRITICAL(&s_lock);
        free(url_copy);
        ESP_LOGW(TAG, "Rejected duplicate OTA request");
        return OTA_MANAGER_START_BUSY;
    }
    s_in_progress = true;
    s_url_copy = url_copy;
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "Accepted OTA request");
    return OTA_MANAGER_START_ACCEPTED;
}

ota_manager_start_result_t ota_manager_complete_start(
    ota_manager_start_result_t result)
{
    if (result != OTA_MANAGER_START_ACCEPTED) {
        if (result == OTA_MANAGER_START_REJECTED_URL) {
            publish_status("rejected", false);
        } else if (result == OTA_MANAGER_START_PREPARE_FAILED ||
                   result == OTA_MANAGER_START_ALLOC_FAILED ||
                   result == OTA_MANAGER_START_TASK_FAILED) {
            publish_status("failed", false);
        }
        return result;
    }

    ota_manager_prepare_fn prepare = NULL;
    void *context = NULL;
    char *url_copy = NULL;
    portENTER_CRITICAL(&s_lock);
    if (s_initialized && s_in_progress) {
        prepare = s_config.prepare;
        context = s_config.callback_context;
        url_copy = s_url_copy;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!prepare || !url_copy) {
        ESP_LOGE(TAG, "Accepted OTA start lost its singleton claim");
        publish_status("failed", false);
        release_claim(url_copy);
        free(url_copy);
        return OTA_MANAGER_START_NOT_INITIALIZED;
    }

    /* Safety preparation must complete before task creation or network I/O. */
    if (!prepare(context)) {
        ESP_LOGE(TAG, "OTA aborted because the safety preparation failed");
        release_claim(url_copy);
        free(url_copy);
        publish_status("failed", false);
        return OTA_MANAGER_START_PREPARE_FAILED;
    }

    TaskHandle_t task = NULL;
    BaseType_t created = xTaskCreatePinnedToCore(
        ota_manager_task, "ota_task", OTA_MANAGER_TASK_STACK_SIZE, url_copy,
        OTA_MANAGER_TASK_PRIORITY, &task, OTA_MANAGER_TASK_CORE);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "OTA task creation failed");
        abort_prepared_update();
        release_claim(url_copy);
        free(url_copy);
        publish_status("failed", false);
        return OTA_MANAGER_START_TASK_FAILED;
    }

    /* The task waits for this notification, avoiding a task-handle race. */
    portENTER_CRITICAL(&s_lock);
    s_task = task;
    portEXIT_CRITICAL(&s_lock);

    // Network status output intentionally occurs before the staged task is
    // released and after the coordinator's MQTT command fence is gone.
    publish_status("starting", false);
    xTaskNotifyGive(task);
    return OTA_MANAGER_START_ACCEPTED;
}

static void ota_manager_task(void *arg)
{
    char *url = (char *)arg;
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    ESP_LOGI(TAG, "Starting verified HTTPS OTA download");
    esp_err_t err = ota_download_verified(url);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA successful, rebooting...");
        publish_status("downloaded_rebooting", true);
        portENTER_CRITICAL(&s_lock);
        if (s_url_copy == url) s_url_copy = NULL;
        portEXIT_CRITICAL(&s_lock);
        free(url);
        vTaskDelay(pdMS_TO_TICKS(OTA_MANAGER_REBOOT_STATUS_GRACE_MS));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
        publish_status("failed", true);
        abort_prepared_update();
        release_claim(url);
        free(url);
    }

    vTaskDelete(NULL);
}

#else

esp_err_t ota_manager_init(const ota_manager_config_t *config)
{
    (void)config;
    return ESP_OK;
}

ota_manager_start_result_t ota_manager_start(const char *url)
{
    (void)url;
    return OTA_MANAGER_START_DISABLED;
}

ota_manager_start_result_t ota_manager_complete_start(
    ota_manager_start_result_t result)
{
    return result;
}

bool ota_manager_url_allowed(const char *url)
{
    (void)url;
    return false;
}

bool ota_manager_is_in_progress(void)
{
    return false;
}

#endif
