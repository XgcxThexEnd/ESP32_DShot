/* Opt-in hardware test: production transport + real ESP-IDF HTTP/TLS stack. */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nvs_flash.h"
#include "ota_transport.h"
#include "protocol_examples_common.h"

static bool run_case(const char *path, int expected_status, bool expected_complete,
                     bool expected_timeout, bool reject_certificate)
{
    char url[256];
    int n = snprintf(url, sizeof(url), "%s%s", CONFIG_OTA_TEST_BASE_URL, path);
    if (n < 0 || (size_t)n >= sizeof(url)) return false;
    int64_t began = esp_timer_get_time();
    esp_transport_handle_t transport = ota_transport_create(began + 3000000, 2000);
    if (!transport) return false;
    esp_http_client_config_t config = {
        .url = url, .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .timeout_ms = 2000, .disable_auto_redirect = true,
        .transport = transport,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { esp_transport_destroy(transport); return false; }
    esp_err_t opened = esp_http_client_open(client, 0);
    int status = 0;
    bool complete = false;
    char body[64];
    unsigned received = 0;
    if (opened == ESP_OK) {
        if (esp_http_client_fetch_headers(client) >= 0) {
            status = esp_http_client_get_status_code(client);
            int count;
            while ((count = esp_http_client_read(client, body, sizeof(body))) > 0) {
                if (status == 200 && strcmp(path, "/ok") == 0 &&
                    (received + (unsigned)count > 2 ||
                     memcmp(body, "OK" + received, (size_t)count) != 0)) {
                    esp_http_client_cleanup(client);
                    esp_transport_destroy(transport);
                    return false;
                }
                received += (unsigned)count;
            }
            complete = esp_http_client_is_complete_data_received(client);
        }
    }
    bool expired = ota_transport_expired(transport);
    int64_t elapsed = esp_timer_get_time() - began;
    esp_http_client_cleanup(client);
    esp_transport_destroy(transport);
    bool passed = reject_certificate ? opened != ESP_OK && !expired
        : status == expected_status && complete == expected_complete &&
          expired == expected_timeout && elapsed < 4500000;
    if (!reject_certificate && strcmp(path, "/ok") == 0) passed &= received == 2;
    printf("OTA_TLS_CASE %s %s status=%d complete=%d timeout=%d elapsed_us=%lld\n",
           path, passed ? "PASS" : "FAIL", status, complete, expired, (long long)elapsed);
    return passed;
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(example_connect());
    esp_sntp_config_t time_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    ESP_ERROR_CHECK(esp_netif_sntp_init(&time_config));
    ESP_ERROR_CHECK(esp_netif_sntp_sync_wait(pdMS_TO_TICKS(30000)));
    bool passed;
#ifdef CONFIG_OTA_TEST_REJECT_CERT
    passed = run_case("/ok", 0, false, false, true);
#else
    passed = run_case("/ok", 200, true, false, false);
    passed &= run_case("/redirect", 302, true, false, false);
    passed &= run_case("/truncated", 200, false, false, false);
    passed &= run_case("/slow-headers", 0, false, true, false);
    passed &= run_case("/slow-body", 200, false, true, false);
#endif
    ESP_LOGI("ota_bench", "OTA_TLS_RESULT %s", passed ? "PASS" : "FAIL");
}
