#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "esp_event.h"
typedef struct test_client *esp_mqtt_client_handle_t;
typedef enum { MQTT_CLIENT_STATE_CONNECTED, MQTT_CLIENT_STATE_WAITING_RECONNECT,
    MQTT_CLIENT_STATE_NOT_STARTED, MQTT_CLIENT_STATE_DISCONNECTED } esp_mqtt_client_connection_state_t;
enum { MQTT_EVENT_CONNECTED, MQTT_EVENT_SUBSCRIBED, MQTT_EVENT_PUBLISHED,
    MQTT_EVENT_DISCONNECTED, MQTT_EVENT_DATA, MQTT_EVENT_ERROR };
enum { MQTT_ERROR_TYPE_SUBSCRIBE_FAILED = 1 };
typedef struct { int error_type; } esp_mqtt_error_codes_t;
typedef struct {
    esp_mqtt_client_handle_t client;
    int total_data_len, current_data_offset, data_len, topic_len, msg_id;
    char *topic, *data;
    bool retain;
    esp_mqtt_error_codes_t *error_handle;
} esp_mqtt_event_t;
typedef esp_mqtt_event_t *esp_mqtt_event_handle_t;
typedef struct {
    struct {
        struct { const char *uri; } address;
        struct { esp_err_t (*crt_bundle_attach)(void *); } verification;
    } broker;
    struct {
        const char *username, *client_id;
        struct { const char *password; } authentication;
    } credentials;
    struct {
        int keepalive;
        struct { const char *topic, *msg; int msg_len, qos; bool retain; } last_will;
    } session;
    struct { int reconnect_timeout_ms, timeout_ms; } network;
    struct { int limit; } outbox;
} esp_mqtt_client_config_t;
esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *config);
esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t client);
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t client);
esp_err_t esp_mqtt_client_disconnect(esp_mqtt_client_handle_t client);
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t client, int event,
                                       esp_event_handler_t handler, void *context);
esp_mqtt_client_connection_state_t esp_mqtt_client_get_state(esp_mqtt_client_handle_t client);
int esp_mqtt_client_get_outbox_size(esp_mqtt_client_handle_t client);
int esp_mqtt_client_subscribe_single(esp_mqtt_client_handle_t client, const char *topic, int qos);
int esp_mqtt_client_publish(esp_mqtt_client_handle_t client, const char *topic,
                            const char *payload, int len, int qos, int retain);

