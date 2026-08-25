#include "command_router.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static command_router_config_t config_all(void)
{
    return (command_router_config_t) {
        .node_base_prefix = "greenhouse/nodes/node-a",
        .legacy_base_prefix = "greenhouse",
        .fan_index_start = 5,
        .fan_count = 2,
        .schedule_slot_count = 4,
        .tach_enabled = true,
        .schedule_enabled = true,
        .ota_enabled = true,
        .legacy_enabled = true,
        .ota_allowed_url_prefix = "https://updates.example.invalid/greenhouse/",
    };
}

static command_router_result_t route(const command_router_config_t *config,
                                     const char *topic, const char *payload,
                                     bool retained,
                                     command_router_command_t *command)
{
    return command_router_parse(config, topic, strlen(topic), payload,
                                strlen(payload), retained, command);
}

static void test_manual_commands(void)
{
    command_router_config_t config = config_all();
    command_router_command_t command;

    assert(route(&config, "greenhouse/nodes/node-a/fan5/set", "ON", false,
                 &command) == COMMAND_ROUTER_ACCEPTED);
    assert(command.type == COMMAND_ROUTER_COMMAND_MANUAL_ON);
    assert(command.fan_local_index == 0 && command.fan_index == 5);
    assert(!command.from_legacy_topic);

    assert(route(&config, "greenhouse/nodes/node-a/fan6/set", "OFF", false,
                 &command) == COMMAND_ROUTER_ACCEPTED);
    assert(command.type == COMMAND_ROUTER_COMMAND_MANUAL_OFF);
    assert(command.fan_local_index == 1);

    assert(route(&config, "greenhouse/nodes/node-a/fan5/percentage/set", "100",
                 false, &command) == COMMAND_ROUTER_ACCEPTED);
    assert(command.type == COMMAND_ROUTER_COMMAND_MANUAL_PERCENTAGE);
    assert(command.value.percentage == 100);

    assert(route(&config, "greenhouse/fan6/percentage/set", "0", false,
                 &command) == COMMAND_ROUTER_ACCEPTED);
    assert(command.from_legacy_topic && command.value.percentage == 0);

    assert(route(&config, "greenhouse/nodes/other/fan5/set", "ON", false,
                 &command) == COMMAND_ROUTER_NO_MATCH);
    assert(route(&config, "greenhouse/nodes/node-a/fan4/set", "ON", false,
                 &command) == COMMAND_ROUTER_REJECTED_OUT_OF_RANGE);
    assert(route(&config, "greenhouse/nodes/node-a/fan7/set", "ON", false,
                 &command) == COMMAND_ROUTER_REJECTED_OUT_OF_RANGE);
    assert(route(&config, "greenhouse/nodes/node-a/fan5/set", "ON", true,
                 &command) == COMMAND_ROUTER_REJECTED_RETAINED);
    assert(route(&config, "greenhouse/nodes/other/fan5/set", "ON", true,
                 &command) == COMMAND_ROUTER_REJECTED_RETAINED);
    assert(route(&config, "greenhouse/nodes/node-a/fan7/set", "ON", true,
                 &command) == COMMAND_ROUTER_REJECTED_RETAINED);
    assert(route(&config, "greenhouse/nodes/node-a/fan5/set", "on", false,
                 &command) == COMMAND_ROUTER_REJECTED_MALFORMED);
    assert(route(&config, "greenhouse/nodes/node-a/fan5/percentage/set", "-1",
                 false, &command) == COMMAND_ROUTER_REJECTED_MALFORMED);
    assert(route(&config, "greenhouse/nodes/node-a/fan5/percentage/set", "+1",
                 false, &command) == COMMAND_ROUTER_REJECTED_MALFORMED);
    assert(route(&config, "greenhouse/nodes/node-a/fan5/percentage/set", " 1",
                 false, &command) == COMMAND_ROUTER_REJECTED_MALFORMED);
    assert(route(&config, "greenhouse/nodes/node-a/fan5/percentage/set", "101",
                 false, &command) == COMMAND_ROUTER_REJECTED_MALFORMED);
    assert(route(&config, "greenhouse/nodes/node-a/fan05/set", "ON", false,
                 &command) == COMMAND_ROUTER_REJECTED_MALFORMED);
    assert(route(&config, "greenhouse/nodes/node-a/fan999999999999999999999/set",
                 "ON", false, &command) == COMMAND_ROUTER_REJECTED_MALFORMED);
    assert(command.type == COMMAND_ROUTER_COMMAND_NONE);
}

static void test_tach_commands(void)
{
    command_router_config_t config = config_all();
    command_router_command_t command;
    assert(route(&config, "greenhouse/nodes/node-a/fan5/alarm/clear", "CLEAR",
                 false, &command) == COMMAND_ROUTER_ACCEPTED);
    assert(command.type == COMMAND_ROUTER_COMMAND_TACH_CLEAR);
    assert(route(&config, "greenhouse/nodes/node-a/fan5/alarm/clear", "clear",
                 false, &command) == COMMAND_ROUTER_REJECTED_MALFORMED);
    assert(route(&config, "greenhouse/fan5/alarm/clear", "CLEAR", false,
                 &command) == COMMAND_ROUTER_NO_MATCH);
    config.tach_enabled = false;
    assert(route(&config, "greenhouse/nodes/node-a/fan5/alarm/clear", "CLEAR",
                 false, &command) == COMMAND_ROUTER_REJECTED_FEATURE_DISABLED);
}

static void test_schedule_commands(void)
{
    command_router_config_t config = config_all();
    command_router_command_t command;

    assert(route(&config, "greenhouse/nodes/node-a/schedule/override", "1",
                 false, &command) == COMMAND_ROUTER_ACCEPTED);
    assert(command.type == COMMAND_ROUTER_COMMAND_SCHEDULE_OVERRIDE);
    assert(command.value.schedule_override.inhibited);
    assert(route(&config, "greenhouse/schedule/override", "0", false,
                 &command) == COMMAND_ROUTER_ACCEPTED);
    assert(command.from_legacy_topic && !command.value.schedule_override.inhibited);

    assert(route(&config, "greenhouse/nodes/node-a/schedule/5/3/set",
                 "60,10,75", false, &command) == COMMAND_ROUTER_ACCEPTED);
    assert(command.type == COMMAND_ROUTER_COMMAND_SCHEDULE_SET);
    assert(command.fan_local_index == 0 && command.schedule_slot == 3);
    assert(command.value.schedule_set.interval_min == 60);
    assert(command.value.schedule_set.duration_min == 10);
    assert(command.value.schedule_set.target_pct == 75);

    assert(route(&config, "greenhouse/nodes/node-a/schedule/6/0/disable", "",
                 false, &command) == COMMAND_ROUTER_ACCEPTED);
    assert(command.type == COMMAND_ROUTER_COMMAND_SCHEDULE_DISABLE);
    assert(route(&config, "greenhouse/nodes/node-a/schedule/6/0/get", "",
                 false, &command) == COMMAND_ROUTER_ACCEPTED);
    assert(command.type == COMMAND_ROUTER_COMMAND_SCHEDULE_GET);

    assert(route(&config, "greenhouse/nodes/node-a/schedule/override", "2",
                 false, &command) == COMMAND_ROUTER_REJECTED_MALFORMED);
    assert(route(&config, "greenhouse/nodes/node-a/schedule/override", " 1",
                 false, &command) == COMMAND_ROUTER_REJECTED_MALFORMED);
    assert(route(&config, "greenhouse/nodes/node-a/schedule/5/4/set", "1,1,1",
                 false, &command) == COMMAND_ROUTER_REJECTED_OUT_OF_RANGE);
    assert(route(&config, "greenhouse/nodes/node-a/schedule/4/0/set", "1,1,1",
                 false, &command) == COMMAND_ROUTER_REJECTED_OUT_OF_RANGE);
    assert(route(&config, "greenhouse/nodes/node-a/schedule/5/0/set", "0,1,1",
                 false, &command) == COMMAND_ROUTER_REJECTED_MALFORMED);
    assert(route(&config, "greenhouse/nodes/node-a/schedule/5/0/set", "10,11,1",
                 false, &command) == COMMAND_ROUTER_REJECTED_MALFORMED);
    assert(route(&config, "greenhouse/nodes/node-a/schedule/5/0/set", "10,1,101",
                 false, &command) == COMMAND_ROUTER_REJECTED_MALFORMED);
    assert(route(&config, "greenhouse/nodes/node-a/schedule/5/0/set", "+10,1,1",
                 false, &command) == COMMAND_ROUTER_REJECTED_MALFORMED);
    assert(route(&config, "greenhouse/nodes/node-a/schedule/5/0/set",
                 "999999999999999999999,1,1", false,
                 &command) == COMMAND_ROUTER_REJECTED_MALFORMED);
    assert(route(&config,
                 "greenhouse/nodes/node-a/schedule/5/999999999999999999999/set",
                 "10,1,1", false,
                 &command) == COMMAND_ROUTER_REJECTED_MALFORMED);
    /* Preserve the original command handler: get/disable ignore payload data. */
    assert(route(&config, "greenhouse/nodes/node-a/schedule/5/0/get", "ignored",
                 false, &command) == COMMAND_ROUTER_ACCEPTED);
    assert(command.type == COMMAND_ROUTER_COMMAND_SCHEDULE_GET);
    assert(route(&config, "greenhouse/nodes/node-a/schedule/5/0/disable", "ignored",
                 false, &command) == COMMAND_ROUTER_ACCEPTED);
    assert(command.type == COMMAND_ROUTER_COMMAND_SCHEDULE_DISABLE);
    assert(route(&config, "greenhouse/nodes/node-a/schedule/5/0/get", "",
                 true, &command) == COMMAND_ROUTER_REJECTED_RETAINED);

    config.schedule_enabled = false;
    assert(route(&config, "greenhouse/nodes/node-a/schedule/override", "1",
                 false, &command) == COMMAND_ROUTER_REJECTED_FEATURE_DISABLED);
}

static void test_ota_commands(void)
{
    command_router_config_t config = config_all();
    command_router_command_t command;
    const char *url = "https://updates.example.invalid/greenhouse/fw.bin";

    assert(route(&config, "greenhouse/nodes/node-a/ota/update", url, false,
                 &command) == COMMAND_ROUTER_ACCEPTED);
    assert(command.type == COMMAND_ROUTER_COMMAND_OTA_UPDATE);
    assert(command.value.ota_update.length == strlen(url));
    assert(strcmp(command.value.ota_update.url, url) == 0);
    assert(route(&config, "greenhouse/ota/update", url, false,
                 &command) == COMMAND_ROUTER_NO_MATCH);
    assert(route(&config, "greenhouse/nodes/node-a/ota/update", url, true,
                 &command) == COMMAND_ROUTER_REJECTED_RETAINED);
    assert(route(&config, "greenhouse/nodes/node-a/ota/update",
                 "https://evil.invalid/fw.bin", false,
                 &command) == COMMAND_ROUTER_REJECTED_MALFORMED);
    assert(route(&config, "greenhouse/nodes/node-a/ota/update",
                 "https://updates.example.invalid/greenhouse/../fw.bin", false,
                 &command) == COMMAND_ROUTER_REJECTED_MALFORMED);
    config.ota_enabled = false;
    assert(route(&config, "greenhouse/nodes/node-a/ota/update", url, false,
                 &command) == COMMAND_ROUTER_REJECTED_FEATURE_DISABLED);
}

static void test_binary_and_size_rejection(void)
{
    command_router_config_t config = config_all();
    command_router_command_t command;
    static const char topic_text[] = "greenhouse/nodes/node-a/fan5/set";
    char unterminated_topic[sizeof(topic_text) - 1];
    const char unterminated_payload[] = {'O', 'N'};
    memcpy(unterminated_topic, topic_text, sizeof(unterminated_topic));
    assert(command_router_parse(&config, unterminated_topic,
                                sizeof(unterminated_topic),
                                unterminated_payload,
                                sizeof(unterminated_payload), false,
                                &command) == COMMAND_ROUTER_ACCEPTED);
    assert(command.type == COMMAND_ROUTER_COMMAND_MANUAL_ON);

    const char topic_with_nul[] = "greenhouse/nodes/node-a/fan5/set\0suffix";
    const char payload_with_nul[] = {'O', 'N', '\0', 'X'};
    assert(command_router_parse(&config, topic_with_nul, sizeof(topic_with_nul) - 1,
                                "ON", 2, false, &command) ==
           COMMAND_ROUTER_REJECTED_MALFORMED);
    assert(command_router_parse(&config, "greenhouse/nodes/node-a/fan5/set",
                                strlen("greenhouse/nodes/node-a/fan5/set"),
                                payload_with_nul, sizeof(payload_with_nul), false,
                                &command) == COMMAND_ROUTER_REJECTED_MALFORMED);

    char huge_topic[COMMAND_ROUTER_TOPIC_CAPACITY] = {0};
    memset(huge_topic, 'a', sizeof(huge_topic));
    assert(command_router_parse(&config, huge_topic, sizeof(huge_topic), "", 0,
                                false, &command) == COMMAND_ROUTER_REJECTED_TOO_LARGE);
    char huge_payload[COMMAND_ROUTER_PAYLOAD_CAPACITY] = {0};
    memset(huge_payload, 'a', sizeof(huge_payload));
    assert(command_router_parse(&config, "greenhouse/nodes/node-a/ota/update",
                                strlen("greenhouse/nodes/node-a/ota/update"),
                                huge_payload, sizeof(huge_payload), false,
                                &command) == COMMAND_ROUTER_REJECTED_TOO_LARGE);
}

static void test_configuration_and_arguments(void)
{
    command_router_config_t config = config_all();
    command_router_command_t command;
    assert(command_router_parse(NULL, "x", 1, "", 0, false, &command) ==
           COMMAND_ROUTER_INVALID_ARGUMENT);
    assert(command_router_parse(&config, NULL, 0, "", 0, false, &command) ==
           COMMAND_ROUTER_INVALID_ARGUMENT);
    assert(command_router_parse(&config, "x", 1, NULL, 1, false, &command) ==
           COMMAND_ROUTER_INVALID_ARGUMENT);
    assert(command_router_parse(&config, "x", 1, NULL, 0, false, &command) ==
           COMMAND_ROUTER_NO_MATCH);

    config.fan_count = 0;
    assert(route(&config, "greenhouse/nodes/node-a/fan5/set", "ON", false,
                 &command) == COMMAND_ROUTER_INVALID_CONFIG);
    config = config_all();
    config.node_base_prefix = "greenhouse/nodes/node-a/";
    assert(route(&config, "greenhouse/nodes/node-a/fan5/set", "ON", false,
                 &command) == COMMAND_ROUTER_INVALID_CONFIG);
    config = config_all();
    config.legacy_base_prefix = config.node_base_prefix;
    assert(route(&config, "greenhouse/nodes/node-a/fan5/set", "ON", false,
                 &command) == COMMAND_ROUTER_INVALID_CONFIG);

    config = config_all();
    config.legacy_enabled = false;
    assert(route(&config, "greenhouse/fan5/set", "ON", false,
                 &command) == COMMAND_ROUTER_NO_MATCH);
}

int main(void)
{
    test_manual_commands();
    test_tach_commands();
    test_schedule_commands();
    test_ota_commands();
    test_binary_and_size_rejection();
    test_configuration_and_arguments();
    puts("command_router host tests passed");
    return 0;
}
