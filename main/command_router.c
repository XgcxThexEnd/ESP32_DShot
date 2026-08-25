#include "command_router.h"

#include <limits.h>
#include <string.h>

#include "controller_policy.h"

#define MAX_SCHEDULE_MINUTES 525600UL

static bool prefix_valid(const char *prefix)
{
    if (!prefix || prefix[0] == '\0') return false;
    size_t length = strlen(prefix);
    return length < COMMAND_ROUTER_TOPIC_CAPACITY && prefix[length - 1] != '/';
}

static bool core_config_valid(const command_router_config_t *config)
{
    if (!config || !prefix_valid(config->node_base_prefix) ||
        config->fan_count < 1 || config->fan_index_start < 1 ||
        config->fan_index_start > INT_MAX - (config->fan_count - 1)) {
        return false;
    }
    if (config->legacy_enabled &&
        (!prefix_valid(config->legacy_base_prefix) ||
         strcmp(config->legacy_base_prefix, config->node_base_prefix) == 0)) {
        return false;
    }
    return true;
}

static const char *relative_topic(const char *topic, const char *prefix)
{
    size_t prefix_len = strlen(prefix);
    if (strncmp(topic, prefix, prefix_len) != 0 || topic[prefix_len] != '/') {
        return NULL;
    }
    return topic + prefix_len + 1;
}

static command_router_result_t reject_retained(bool retained)
{
    return retained ? COMMAND_ROUTER_REJECTED_RETAINED : COMMAND_ROUTER_ACCEPTED;
}

static command_router_result_t assign_fan(
    const command_router_config_t *config,
    unsigned long fan_value,
    command_router_command_t *command)
{
    unsigned long first = (unsigned long)config->fan_index_start;
    unsigned long last = first + (unsigned long)config->fan_count - 1UL;
    if (fan_value < first || fan_value > last) {
        return COMMAND_ROUTER_REJECTED_OUT_OF_RANGE;
    }
    command->fan_index = (int)fan_value;
    command->fan_local_index = (int)(fan_value - first);
    return COMMAND_ROUTER_ACCEPTED;
}

static command_router_result_t parse_fan_command(
    const command_router_config_t *config,
    const char *relative,
    const char *payload,
    bool retained,
    bool legacy,
    command_router_command_t *command)
{
    if (strncmp(relative, "fan", 3) != 0) return COMMAND_ROUTER_NO_MATCH;

    const char *cursor = relative + 3;
    /* Manual/tach topics were historically generated with canonical fan%d. */
    if (cursor[0] == '0' && cursor[1] >= '0' && cursor[1] <= '9') {
        return COMMAND_ROUTER_REJECTED_MALFORMED;
    }
    unsigned long fan_value = 0;
    if (!controller_parse_uint_field(&cursor, INT_MAX, '/', &fan_value)) {
        return COMMAND_ROUTER_REJECTED_MALFORMED;
    }

    command_router_result_t fan_result = assign_fan(config, fan_value, command);
    if (fan_result != COMMAND_ROUTER_ACCEPTED) return fan_result;

    if (strcmp(cursor, "set") == 0) {
        command_router_result_t retained_result = reject_retained(retained);
        if (retained_result != COMMAND_ROUTER_ACCEPTED) return retained_result;
        if (strcmp(payload, "ON") == 0) {
            command->type = COMMAND_ROUTER_COMMAND_MANUAL_ON;
        } else if (strcmp(payload, "OFF") == 0) {
            command->type = COMMAND_ROUTER_COMMAND_MANUAL_OFF;
        } else {
            return COMMAND_ROUTER_REJECTED_MALFORMED;
        }
        return COMMAND_ROUTER_ACCEPTED;
    }

    if (strcmp(cursor, "percentage/set") == 0) {
        command_router_result_t retained_result = reject_retained(retained);
        if (retained_result != COMMAND_ROUTER_ACCEPTED) return retained_result;
        uint8_t percentage = 0;
        if (!controller_parse_percent(payload, &percentage)) {
            return COMMAND_ROUTER_REJECTED_MALFORMED;
        }
        command->type = COMMAND_ROUTER_COMMAND_MANUAL_PERCENTAGE;
        command->value.percentage = percentage;
        return COMMAND_ROUTER_ACCEPTED;
    }

    if (!legacy && strcmp(cursor, "alarm/clear") == 0) {
        if (!config->tach_enabled) return COMMAND_ROUTER_REJECTED_FEATURE_DISABLED;
        command_router_result_t retained_result = reject_retained(retained);
        if (retained_result != COMMAND_ROUTER_ACCEPTED) return retained_result;
        if (strcmp(payload, "CLEAR") != 0) {
            return COMMAND_ROUTER_REJECTED_MALFORMED;
        }
        command->type = COMMAND_ROUTER_COMMAND_TACH_CLEAR;
        return COMMAND_ROUTER_ACCEPTED;
    }

    return COMMAND_ROUTER_NO_MATCH;
}

static bool parse_schedule_payload(const char *payload,
                                   uint32_t *interval,
                                   uint32_t *duration,
                                   uint8_t *target)
{
    const char *cursor = payload;
    unsigned long parsed_interval = 0;
    unsigned long parsed_duration = 0;
    unsigned long parsed_target = 0;
    if (!controller_parse_uint_field(&cursor, MAX_SCHEDULE_MINUTES, ',',
                                     &parsed_interval) ||
        parsed_interval < 1 ||
        !controller_parse_uint_field(&cursor, MAX_SCHEDULE_MINUTES, ',',
                                     &parsed_duration) ||
        parsed_duration < 1 ||
        !controller_parse_uint_field(&cursor, 100, '\0', &parsed_target) ||
        parsed_duration > parsed_interval) {
        return false;
    }
    *interval = (uint32_t)parsed_interval;
    *duration = (uint32_t)parsed_duration;
    *target = (uint8_t)parsed_target;
    return true;
}

static command_router_result_t parse_schedule_command(
    const command_router_config_t *config,
    const char *relative,
    const char *payload,
    bool retained,
    command_router_command_t *command)
{
    static const char schedule_prefix[] = "schedule/";
    if (strncmp(relative, schedule_prefix, sizeof(schedule_prefix) - 1) != 0) {
        return COMMAND_ROUTER_NO_MATCH;
    }
    if (!config->schedule_enabled) {
        return COMMAND_ROUTER_REJECTED_FEATURE_DISABLED;
    }
    if (config->schedule_slot_count == 0 || config->schedule_slot_count > INT_MAX) {
        return COMMAND_ROUTER_INVALID_CONFIG;
    }

    const char *cursor = relative + sizeof(schedule_prefix) - 1;
    if (strcmp(cursor, "override") == 0) {
        command_router_result_t retained_result = reject_retained(retained);
        if (retained_result != COMMAND_ROUTER_ACCEPTED) return retained_result;
        unsigned long value = 0;
        if (!controller_parse_uint_field(&payload, 1, '\0', &value)) {
            return COMMAND_ROUTER_REJECTED_MALFORMED;
        }
        command->type = COMMAND_ROUTER_COMMAND_SCHEDULE_OVERRIDE;
        command->value.schedule_override.inhibited = value != 0;
        return COMMAND_ROUTER_ACCEPTED;
    }

    unsigned long fan_value = 0;
    unsigned long slot_value = 0;
    if (!controller_parse_uint_field(&cursor, INT_MAX, '/', &fan_value) ||
        !controller_parse_uint_field(&cursor, INT_MAX, '/', &slot_value)) {
        return COMMAND_ROUTER_REJECTED_MALFORMED;
    }
    command_router_result_t fan_result = assign_fan(config, fan_value, command);
    if (fan_result != COMMAND_ROUTER_ACCEPTED) return fan_result;
    if (slot_value >= config->schedule_slot_count) {
        return COMMAND_ROUTER_REJECTED_OUT_OF_RANGE;
    }
    command->schedule_slot = (unsigned int)slot_value;

    if (strcmp(cursor, "set") == 0) {
        command_router_result_t retained_result = reject_retained(retained);
        if (retained_result != COMMAND_ROUTER_ACCEPTED) return retained_result;
        uint32_t interval = 0;
        uint32_t duration = 0;
        uint8_t target = 0;
        if (!parse_schedule_payload(payload, &interval, &duration, &target)) {
            return COMMAND_ROUTER_REJECTED_MALFORMED;
        }
        command->type = COMMAND_ROUTER_COMMAND_SCHEDULE_SET;
        command->value.schedule_set.interval_min = interval;
        command->value.schedule_set.duration_min = duration;
        command->value.schedule_set.target_pct = target;
        return COMMAND_ROUTER_ACCEPTED;
    }

    if (strcmp(cursor, "disable") == 0 || strcmp(cursor, "get") == 0) {
        command_router_result_t retained_result = reject_retained(retained);
        if (retained_result != COMMAND_ROUTER_ACCEPTED) return retained_result;
        command->type = strcmp(cursor, "disable") == 0
                            ? COMMAND_ROUTER_COMMAND_SCHEDULE_DISABLE
                            : COMMAND_ROUTER_COMMAND_SCHEDULE_GET;
        return COMMAND_ROUTER_ACCEPTED;
    }

    return COMMAND_ROUTER_NO_MATCH;
}

static command_router_result_t parse_ota_command(
    const command_router_config_t *config,
    const char *relative,
    const char *payload,
    size_t payload_len,
    bool retained,
    bool legacy,
    command_router_command_t *command)
{
    if (strcmp(relative, "ota/update") != 0 || legacy) {
        return COMMAND_ROUTER_NO_MATCH;
    }
    if (!config->ota_enabled) return COMMAND_ROUTER_REJECTED_FEATURE_DISABLED;
    command_router_result_t retained_result = reject_retained(retained);
    if (retained_result != COMMAND_ROUTER_ACCEPTED) return retained_result;
    if (!config->ota_allowed_url_prefix ||
        !controller_https_url_allowed(payload, config->ota_allowed_url_prefix)) {
        return COMMAND_ROUTER_REJECTED_MALFORMED;
    }
    command->type = COMMAND_ROUTER_COMMAND_OTA_UPDATE;
    memcpy(command->value.ota_update.url, payload, payload_len + 1);
    command->value.ota_update.length = payload_len;
    return COMMAND_ROUTER_ACCEPTED;
}

command_router_result_t command_router_parse(
    const command_router_config_t *config,
    const char *topic,
    size_t topic_len,
    const void *payload_bytes,
    size_t payload_len,
    bool retained,
    command_router_command_t *out_command)
{
    if (out_command) {
        memset(out_command, 0, sizeof(*out_command));
        out_command->fan_local_index = -1;
    }
    if (!config || !topic || !out_command || (!payload_bytes && payload_len != 0)) {
        return COMMAND_ROUTER_INVALID_ARGUMENT;
    }
    if (!core_config_valid(config)) return COMMAND_ROUTER_INVALID_CONFIG;
    if (topic_len == 0) return COMMAND_ROUTER_REJECTED_MALFORMED;
    if (topic_len >= COMMAND_ROUTER_TOPIC_CAPACITY ||
        payload_len >= COMMAND_ROUTER_PAYLOAD_CAPACITY) {
        return COMMAND_ROUTER_REJECTED_TOO_LARGE;
    }
    if (memchr(topic, '\0', topic_len) != NULL ||
        (payload_len > 0 && memchr(payload_bytes, '\0', payload_len) != NULL)) {
        return COMMAND_ROUTER_REJECTED_MALFORMED;
    }
    /* Match the existing handler's global retained-command safety gate. */
    if (retained) return COMMAND_ROUTER_REJECTED_RETAINED;

    char topic_text[COMMAND_ROUTER_TOPIC_CAPACITY];
    char payload[COMMAND_ROUTER_PAYLOAD_CAPACITY];
    memcpy(topic_text, topic, topic_len);
    topic_text[topic_len] = '\0';
    if (payload_len > 0) memcpy(payload, payload_bytes, payload_len);
    payload[payload_len] = '\0';

    bool legacy = false;
    const char *relative = relative_topic(topic_text, config->node_base_prefix);
    if (!relative && config->legacy_enabled) {
        relative = relative_topic(topic_text, config->legacy_base_prefix);
        legacy = relative != NULL;
    }
    if (!relative || relative[0] == '\0') return COMMAND_ROUTER_NO_MATCH;

    command_router_command_t command = {
        .type = COMMAND_ROUTER_COMMAND_NONE,
        .fan_local_index = -1,
        .from_legacy_topic = legacy,
    };
    command_router_result_t result = parse_fan_command(
        config, relative, payload, retained, legacy, &command);
    if (result == COMMAND_ROUTER_NO_MATCH) {
        result = parse_schedule_command(config, relative, payload, retained, &command);
    }
    if (result == COMMAND_ROUTER_NO_MATCH) {
        result = parse_ota_command(config, relative, payload, payload_len,
                                   retained, legacy, &command);
    }
    if (result == COMMAND_ROUTER_ACCEPTED) {
        *out_command = command;
    }
    return result;
}
