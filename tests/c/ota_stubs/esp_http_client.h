#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_transport.h"
typedef struct fake_http_client *esp_http_client_handle_t;
typedef struct {
    const char *url;
    int timeout_ms;
    bool disable_auto_redirect;
    esp_err_t (*crt_bundle_attach)(void *config);
    esp_transport_handle_t transport;
} esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int length);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);
