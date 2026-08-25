#include "app_config.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "This firmware currently supports only ESP32-S3 targets"
#endif

#include "controller_policy.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_mac.h"

#ifndef CONFIG_FAN_COUNT
#define CONFIG_FAN_COUNT 2
#endif
#ifndef CONFIG_FAN_INDEX_START
#define CONFIG_FAN_INDEX_START 1
#endif
#ifndef CONFIG_DSHOT_GPIO
#define CONFIG_DSHOT_GPIO "5,7"
#endif
#ifndef CONFIG_MIN_SPIN_PCT
#define CONFIG_MIN_SPIN_PCT 15
#endif
#ifndef CONFIG_RAMP_STEP_PCT
#define CONFIG_RAMP_STEP_PCT 3
#endif
#ifndef CONFIG_RAMP_TICK_MS
#define CONFIG_RAMP_TICK_MS 20
#endif
#ifndef CONFIG_NODE_ID
#define CONFIG_NODE_ID ""
#endif
#ifndef CONFIG_MQTT_ROOT_TOPIC
#define CONFIG_MQTT_ROOT_TOPIC "greenhouse_esp"
#endif

#if CONFIG_FAN_COUNT > APP_CONFIG_MAX_FANS
#error "ESP32-S3 provides only four RMT TX channels; CONFIG_FAN_COUNT must be <= 4"
#endif

#define TOPIC_SUFFIX_CAPACITY 112

static app_config_t s_config;
static bool s_initialized;
static const char *TAG = "greenhouse";

static const char *skip_space(const char *p)
{
    while (p && isspace((unsigned char)*p)) ++p;
    return p;
}

static int parse_dshot_gpios(app_config_t *config)
{
    if (config->fan_count < 1 || config->fan_count > APP_CONFIG_MAX_FANS) {
        ESP_LOGE("config", "FAN_COUNT=%d exceeds ESP32-S3 RMT TX capacity %d",
                 config->fan_count, APP_CONFIG_MAX_FANS);
        return -1;
    }

    const char *cursor = CONFIG_DSHOT_GPIO;
    int count = 0;
    while (true) {
        if (count >= APP_CONFIG_MAX_FANS) {
            ESP_LOGE("config", "DSHOT_GPIO contains more than %d entries",
                     APP_CONFIG_MAX_FANS);
            return -1;
        }

        const char *field = skip_space(cursor);
        errno = 0;
        char *end = NULL;
        long gpio_value = strtol(field, &end, 10);
        if (errno == ERANGE || end == field || gpio_value < 0 || gpio_value > INT_MAX) {
            ESP_LOGE("config", "Invalid GPIO token near '%s'", field);
            return -1;
        }
        end = (char *)skip_space(end);
        if (*end != '\0' && *end != ',') {
            ESP_LOGE("config", "Unexpected text in DSHOT_GPIO near '%s'", end);
            return -1;
        }

        int gpio = (int)gpio_value;
        if (!GPIO_IS_VALID_OUTPUT_GPIO(gpio)) {
            ESP_LOGE("config", "GPIO%d is not a valid output GPIO on this target", gpio);
            return -1;
        }
#ifdef CONFIG_MOTOR_INTERLOCK_ENABLED
        if (gpio == CONFIG_MOTOR_INTERLOCK_GPIO) {
            ESP_LOGE("config", "GPIO%d cannot be both DShot and motor interlock", gpio);
            return -1;
        }
#endif
        for (int i = 0; i < count; ++i) {
            if (config->dshot_gpios[i] == gpio) {
                ESP_LOGE("config", "GPIO%d is assigned to more than one fan", gpio);
                return -1;
            }
        }

        config->dshot_gpios[count++] = gpio;
        if (*end == '\0') break;
        cursor = end + 1;
        if (*skip_space(cursor) == '\0') {
            ESP_LOGE("config", "DSHOT_GPIO has a trailing comma");
            return -1;
        }
    }

    if (count != config->fan_count) {
        ESP_LOGE("config", "DSHOT_GPIO has %d entries but FAN_COUNT=%d",
                 count, config->fan_count);
        return -1;
    }
    return count;
}

#ifdef CONFIG_TACH_FEEDBACK_ENABLED
static int parse_tach_gpios(app_config_t *config)
{
    const char *cursor = CONFIG_TACH_GPIO;
    int count = 0;
    while (true) {
        if (count >= APP_CONFIG_MAX_FANS) {
            ESP_LOGE("config", "TACH_GPIO contains more than %d entries",
                     APP_CONFIG_MAX_FANS);
            return -1;
        }

        const char *field = skip_space(cursor);
        errno = 0;
        char *end = NULL;
        long value = strtol(field, &end, 10);
        if (errno == ERANGE || end == field || value < 0 || value > INT_MAX) {
            ESP_LOGE("config", "Invalid tach GPIO token near '%s'", field);
            return -1;
        }
        end = (char *)skip_space(end);
        if (*end != '\0' && *end != ',') return -1;

        int gpio = (int)value;
        if (!GPIO_IS_VALID_GPIO(gpio)) {
            ESP_LOGE("config", "GPIO%d is not a valid tach input", gpio);
            return -1;
        }
        for (int i = 0; i < count; ++i) {
            if (config->tach_gpios[i] == gpio) {
                ESP_LOGE("config", "Tach GPIO%d is assigned twice", gpio);
                return -1;
            }
        }
        for (int i = 0; i < config->fan_count; ++i) {
            if (config->dshot_gpios[i] == gpio) {
                ESP_LOGE("config", "GPIO%d cannot be both DShot and tach", gpio);
                return -1;
            }
        }
#ifdef CONFIG_MOTOR_INTERLOCK_ENABLED
        if (gpio == CONFIG_MOTOR_INTERLOCK_GPIO) {
            ESP_LOGE("config", "GPIO%d cannot be both tach and motor interlock", gpio);
            return -1;
        }
#endif

        config->tach_gpios[count++] = gpio;
        if (*end == '\0') break;
        cursor = end + 1;
        if (*skip_space(cursor) == '\0') return -1;
    }

    if (count != config->fan_count) {
        ESP_LOGE("config", "TACH_GPIO has %d entries but FAN_COUNT=%d",
                 count, config->fan_count);
        return -1;
    }
    return count;
}
#endif

static bool initialize_node_identity(app_config_t *config)
{
#ifdef CONFIG_MQTT_LEGACY_TOPICS
    if (config->fan_index_start < 1 ||
        config->fan_index_start + config->fan_count - 1 > 8) {
        ESP_LOGE("config", "Legacy topics require fan numbers to remain in the global 1..8 range");
        return false;
    }
#endif
    if (!controller_identifier_valid(CONFIG_NODE_ID, true) ||
        strlen(CONFIG_NODE_ID) >= sizeof(config->node_id) ||
        !controller_topic_level_valid(CONFIG_MQTT_ROOT_TOPIC)) {
        ESP_LOGE("config", "NODE_ID/MQTT_ROOT_TOPIC contains invalid characters or is too long");
        return false;
    }

    if (CONFIG_NODE_ID[0] != '\0') {
        strlcpy(config->node_id, CONFIG_NODE_ID, sizeof(config->node_id));
    } else {
        uint8_t mac[6] = {0};
        if (esp_read_mac(mac, ESP_MAC_BASE) != ESP_OK) return false;
        snprintf(config->node_id, sizeof(config->node_id),
                 "esp32s3_%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    if (!controller_build_node_topic(config->node_topic, sizeof(config->node_topic),
                                     CONFIG_MQTT_ROOT_TOPIC, config->node_id)) {
        return false;
    }
    strlcpy(config->mqtt_root_topic, CONFIG_MQTT_ROOT_TOPIC,
            sizeof(config->mqtt_root_topic));
    ESP_LOGI("config", "Node identity: %s", config->node_id);
    return true;
}

static void fingerprint_bytes(uint64_t *hash, const void *data, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    for (size_t i = 0; i < length; ++i) {
        *hash = (*hash ^ bytes[i]) * UINT64_C(1099511628211);
    }
}

static void fingerprint_u32(uint64_t *hash, uint32_t value)
{
    // Use an explicit byte order so the persisted identity is architecture
    // independent and remains stable across toolchain changes.
    uint8_t encoded[4] = {
        (uint8_t)value,
        (uint8_t)(value >> 8),
        (uint8_t)(value >> 16),
        (uint8_t)(value >> 24),
    };
    fingerprint_bytes(hash, encoded, sizeof(encoded));
}

static uint64_t topology_fingerprint(const app_config_t *config)
{
    static const char domain[] = "ESP32_DShot/topology/v1";
    uint64_t hash = UINT64_C(14695981039346656037);

    fingerprint_bytes(&hash, domain, sizeof(domain) - 1U);
    fingerprint_u32(&hash, (uint32_t)strlen(config->node_id));
    fingerprint_bytes(&hash, config->node_id, strlen(config->node_id));
    fingerprint_u32(&hash, (uint32_t)config->fan_count);
    fingerprint_u32(&hash, (uint32_t)config->fan_index_start);
    for (int i = 0; i < config->fan_count; ++i) {
        fingerprint_u32(&hash, (uint32_t)config->dshot_gpios[i]);
    }
#ifdef CONFIG_TACH_FEEDBACK_ENABLED
    const uint8_t tach_enabled = 1;
    fingerprint_bytes(&hash, &tach_enabled, sizeof(tach_enabled));
    for (int i = 0; i < config->fan_count; ++i) {
        fingerprint_u32(&hash, (uint32_t)config->tach_gpios[i]);
    }
#else
    const uint8_t tach_enabled = 0;
    fingerprint_bytes(&hash, &tach_enabled, sizeof(tach_enabled));
#endif
    return hash;
}

esp_err_t app_config_init(void)
{
    if (s_initialized) return ESP_OK;

    app_config_t candidate = {
        .fan_count = CONFIG_FAN_COUNT,
        .fan_index_start = CONFIG_FAN_INDEX_START,
        .min_spin_pct = CONFIG_MIN_SPIN_PCT,
        .ramp_step_pct = CONFIG_RAMP_STEP_PCT,
        .ramp_tick_ms = CONFIG_RAMP_TICK_MS,
    };

    if (parse_dshot_gpios(&candidate) != candidate.fan_count ||
        !initialize_node_identity(&candidate)) {
        ESP_LOGE(TAG, "Configuration invalid; leaving motor outputs unconfigured");
        return ESP_ERR_INVALID_ARG;
    }
#ifdef CONFIG_TACH_FEEDBACK_ENABLED
    if (parse_tach_gpios(&candidate) != candidate.fan_count) {
        ESP_LOGE(TAG, "Tachometer configuration invalid");
        return ESP_ERR_INVALID_ARG;
    }
#endif

    candidate.topology_fingerprint = topology_fingerprint(&candidate);
    ESP_LOGI("config", "Topology fingerprint: %016" PRIx64,
             candidate.topology_fingerprint);

    s_config = candidate;
    s_initialized = true;
    return ESP_OK;
}

const app_config_t *app_config_get(void)
{
    return s_initialized ? &s_config : NULL;
}

static bool format_topic(char *out, size_t out_size, const char *prefix,
                         const char *suffix_format, va_list args)
{
    if (!out || out_size == 0 || !prefix || prefix[0] == '\0' || !suffix_format) {
        return false;
    }

    char suffix[TOPIC_SUFFIX_CAPACITY];
    int suffix_len = vsnprintf(suffix, sizeof(suffix), suffix_format, args);
    if (suffix_len < 0 || (size_t)suffix_len >= sizeof(suffix)) return false;
    int written = snprintf(out, out_size, "%s/%s", prefix, suffix);
    return written > 0 && (size_t)written < out_size;
}

bool app_config_format_node_topic(char *out, size_t out_size,
                                  const char *suffix_format, ...)
{
    if (!s_initialized) return false;
    va_list args;
    va_start(args, suffix_format);
    bool valid = format_topic(out, out_size, s_config.node_topic,
                              suffix_format, args);
    va_end(args);
    return valid;
}

#ifdef CONFIG_MQTT_LEGACY_TOPICS
bool app_config_format_legacy_topic(char *out, size_t out_size,
                                    const char *suffix_format, ...)
{
    if (!s_initialized) return false;
    va_list args;
    va_start(args, suffix_format);
    bool valid = format_topic(out, out_size, s_config.mqtt_root_topic,
                              suffix_format, args);
    va_end(args);
    return valid;
}
#endif
