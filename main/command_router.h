#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COMMAND_ROUTER_TOPIC_CAPACITY 192
#define COMMAND_ROUTER_PAYLOAD_CAPACITY 768

typedef enum {
    COMMAND_ROUTER_ACCEPTED = 0,
    COMMAND_ROUTER_NO_MATCH,
    COMMAND_ROUTER_REJECTED_RETAINED,
    COMMAND_ROUTER_REJECTED_MALFORMED,
    COMMAND_ROUTER_REJECTED_OUT_OF_RANGE,
    COMMAND_ROUTER_REJECTED_FEATURE_DISABLED,
    COMMAND_ROUTER_REJECTED_TOO_LARGE,
    COMMAND_ROUTER_INVALID_ARGUMENT,
    COMMAND_ROUTER_INVALID_CONFIG,
} command_router_result_t;

typedef enum {
    COMMAND_ROUTER_COMMAND_NONE = 0,
    COMMAND_ROUTER_COMMAND_MANUAL_ON,
    COMMAND_ROUTER_COMMAND_MANUAL_OFF,
    COMMAND_ROUTER_COMMAND_MANUAL_PERCENTAGE,
    COMMAND_ROUTER_COMMAND_TACH_CLEAR,
    COMMAND_ROUTER_COMMAND_SCHEDULE_OVERRIDE,
    COMMAND_ROUTER_COMMAND_SCHEDULE_SET,
    COMMAND_ROUTER_COMMAND_SCHEDULE_DISABLE,
    COMMAND_ROUTER_COMMAND_SCHEDULE_GET,
    COMMAND_ROUTER_COMMAND_OTA_UPDATE,
} command_router_command_type_t;

typedef struct {
    const char *node_base_prefix;   /* <root>/nodes/<node-id>, without trailing slash */
    const char *legacy_base_prefix; /* <root>, without trailing slash */
    int fan_index_start;
    int fan_count;
    unsigned int schedule_slot_count;
    bool tach_enabled;
    bool schedule_enabled;
    bool ota_enabled;
    bool legacy_enabled;
    const char *ota_allowed_url_prefix;
} command_router_config_t;

typedef struct {
    command_router_command_type_t type;
    int fan_local_index; /* zero-based, or -1 for commands without a fan */
    int fan_index;       /* external/displayed fan index, or 0 when not applicable */
    unsigned int schedule_slot;
    bool from_legacy_topic;
    union {
        uint8_t percentage;
        struct {
            bool inhibited;
        } schedule_override;
        struct {
            uint32_t interval_min;
            uint32_t duration_min;
            uint8_t target_pct;
        } schedule_set;
        struct {
            char url[COMMAND_ROUTER_PAYLOAD_CAPACITY];
            size_t length;
        } ota_update;
    } value;
} command_router_command_t;

/**
 * Parse one completely assembled MQTT message without performing side effects.
 *
 * topic_len and payload_len exclude any terminator. Payload may be NULL only
 * when payload_len is zero. Embedded NUL bytes are rejected. On every result
 * other than COMMAND_ROUTER_ACCEPTED, out_command is reset to type NONE.
 */
command_router_result_t command_router_parse(
    const command_router_config_t *config,
    const char *topic,
    size_t topic_len,
    const void *payload,
    size_t payload_len,
    bool retained,
    command_router_command_t *out_command);

#ifdef __cplusplus
}
#endif
