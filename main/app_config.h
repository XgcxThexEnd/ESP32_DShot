#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_CONFIG_MAX_FANS 4
#define APP_CONFIG_NODE_ID_CAPACITY 49
#define APP_CONFIG_TOPIC_CAPACITY 128

/**
 * Validated, immutable runtime configuration.
 *
 * app_config_init() populates the singleton before application tasks start.
 * Consumers receive only a const pointer and must not retain writable aliases.
 * Fan positions in the GPIO arrays are local zero-based indexes; MQTT-facing
 * fan numbers begin at fan_index_start.
 */
typedef struct {
    int fan_count;
    int fan_index_start;
    int min_spin_pct;
    int ramp_step_pct;
    int ramp_tick_ms;
    int dshot_gpios[APP_CONFIG_MAX_FANS];
#ifdef CONFIG_TACH_FEEDBACK_ENABLED
    int tach_gpios[APP_CONFIG_MAX_FANS];
#endif
    /**
     * Deterministic identity for the node's ordered motor topology.
     *
     * This includes the node ID, fan count/index range, ordered DShot GPIOs,
     * and (when enabled) the ordered tach GPIOs. Persistent state must match
     * this value before it can authorize a nonzero restore.
     */
    uint64_t topology_fingerprint;
    char mqtt_root_topic[APP_CONFIG_TOPIC_CAPACITY];
    char node_id[APP_CONFIG_NODE_ID_CAPACITY];
    char node_topic[APP_CONFIG_TOPIC_CAPACITY];
} app_config_t;

/** Validate Kconfig values, derive the node identity, and freeze configuration. */
esp_err_t app_config_init(void);

/** Return the immutable configuration, or NULL before successful initialization. */
const app_config_t *app_config_get(void);

/** Format <root>/nodes/<node-id>/<suffix>. Requires successful initialization. */
bool app_config_format_node_topic(char *out, size_t out_size,
                                  const char *suffix_format, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

#ifdef CONFIG_MQTT_LEGACY_TOPICS
/** Format <root>/<suffix> for the bounded legacy-topic migration path. */
bool app_config_format_legacy_topic(char *out, size_t out_size,
                                    const char *suffix_format, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;
#endif

#ifdef __cplusplus
}
#endif
