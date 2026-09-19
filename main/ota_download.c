#include "ota_download.h"

#include "sdkconfig.h"

#ifdef CONFIG_OTA_ENABLED

#include <stdint.h>
#include <stdlib.h>

#include "controller_policy.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "ota_transport.h"

#define OTA_DOWNLOAD_BUFFER_SIZE 4096
#define OTA_DOWNLOAD_IDLE_TIMEOUT_MS 10000
#define OTA_DOWNLOAD_DEADLINE_US (INT64_C(300) * 1000000)

esp_err_t ota_download_verified(const char *url)
{
    if (!controller_https_url_allowed(url, CONFIG_OTA_ALLOWED_URL_PREFIX)) {
        return ESP_ERR_INVALID_ARG;
    }

    const int64_t deadline_us = esp_timer_get_time() + OTA_DOWNLOAD_DEADLINE_US;
    esp_err_t result = ESP_ERR_NO_MEM;
    esp_ota_handle_t ota = 0;
    bool ota_open = false;
    esp_http_client_handle_t client = NULL;
    esp_transport_handle_t transport = NULL;
    char *buffer = malloc(OTA_DOWNLOAD_BUFFER_SIZE);
    if (!buffer) return result;

    transport = ota_transport_create(deadline_us, OTA_DOWNLOAD_IDLE_TIMEOUT_MS);
    if (!transport) goto cleanup;

    const esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = OTA_DOWNLOAD_IDLE_TIMEOUT_MS,
        .disable_auto_redirect = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .transport = transport,
    };
    client = esp_http_client_init(&config);
    if (!client) goto cleanup;

    result = esp_http_client_open(client, 0);
    if (result != ESP_OK) goto cleanup;
    int64_t content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        result = content_length == -ESP_ERR_HTTP_EAGAIN
                     ? ESP_ERR_TIMEOUT : ESP_FAIL;
        goto cleanup;
    }
    // Streaming HTTP APIs never follow redirects themselves. Reject every
    // non-200 response before allocating an OTA handle or writing flash.
    if (esp_http_client_get_status_code(client) != 200) {
        result = ESP_FAIL;
        goto cleanup;
    }

    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (!partition) {
        result = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }
    if (content_length > (int64_t)partition->size) {
        result = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }
    if (esp_timer_get_time() >= deadline_us) {
        result = ESP_ERR_TIMEOUT;
        goto cleanup;
    }
    result = esp_ota_begin(partition, OTA_SIZE_UNKNOWN, &ota);
    if (result != ESP_OK) goto cleanup;
    ota_open = true;

    size_t written = 0;
    for (;;) {
        if (esp_timer_get_time() >= deadline_us) {
            result = ESP_ERR_TIMEOUT;
            goto cleanup;
        }
        int received = esp_http_client_read(client, buffer,
                                            OTA_DOWNLOAD_BUFFER_SIZE);
        if (received < 0) {
            result = received == -ESP_ERR_HTTP_EAGAIN
                         ? ESP_ERR_TIMEOUT : ESP_FAIL;
            goto cleanup;
        }
        if (received == 0) {
            if (!esp_http_client_is_complete_data_received(client) ||
                written == 0 ||
                (content_length > 0 && written != (size_t)content_length)) {
                result = ESP_FAIL;
                goto cleanup;
            }
            break;
        }
        if ((size_t)received > partition->size - written) {
            result = ESP_ERR_INVALID_SIZE;
            goto cleanup;
        }
        if (esp_timer_get_time() >= deadline_us) {
            result = ESP_ERR_TIMEOUT;
            goto cleanup;
        }
        result = esp_ota_write(ota, buffer, (size_t)received);
        if (result != ESP_OK) goto cleanup;
        written += (size_t)received;
    }

    // esp_ota_end validates the image (including configured signature checks)
    // and consumes the handle on both success and failure.
    result = esp_ota_end(ota);
    ota_open = false;
    if (result != ESP_OK) goto cleanup;
    if (esp_timer_get_time() >= deadline_us) {
        result = ESP_ERR_TIMEOUT;
        goto cleanup;
    }
    result = esp_ota_set_boot_partition(partition);

cleanup:
    if (ota_open) (void)esp_ota_abort(ota);
    if (client) (void)esp_http_client_cleanup(client);
    // Custom transports are caller-owned; close the HTTP client first.
    if (transport) {
        if (ota_transport_expired(transport)) result = ESP_ERR_TIMEOUT;
        (void)esp_transport_destroy(transport);
    }
    free(buffer);
    return result;
}

#endif
