idf_component_register(
    SRCS "ota_transport_bench.c" "../../../../../main/ota_transport.c"
    INCLUDE_DIRS "../../../../../main"
    REQUIRES esp_http_client esp_timer tcp_transport mbedtls nvs_flash
             esp_netif esp_event protocol_examples_common)
