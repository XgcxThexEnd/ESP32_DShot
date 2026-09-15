#pragma once
#include "esp_transport.h"
#define ESP_TLS_ERR_SSL_WANT_READ (-0x6900)
#define ESP_TLS_ERR_SSL_WANT_WRITE (-0x6880)
esp_transport_handle_t esp_transport_ssl_init(void);
void esp_transport_ssl_crt_bundle_attach(esp_transport_handle_t transport,
                                        esp_err_t (*attach)(void *));
