#include "controller_policy.h"
#include "dshot_protocol.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static void test_identifiers(void)
{
    assert(!controller_identifier_valid(NULL, false));
    assert(!controller_identifier_valid("", false));
    assert(controller_identifier_valid("", true));
    assert(controller_identifier_valid("node-01_A", false));
    assert(controller_topic_level_valid("greenhouse_esp"));
    assert(!controller_topic_level_valid("nodes/site"));
    assert(!controller_topic_level_valid("node+"));
    assert(!controller_topic_level_valid("node#"));
    assert(!controller_topic_level_valid("node.example"));
    assert(!controller_topic_level_valid("node space"));
}

static void test_node_topics(void)
{
    char topic[64] = {0};
    assert(controller_build_node_topic(topic, sizeof(topic), "greenhouse_esp", "node-1"));
    assert(strcmp(topic, "greenhouse_esp/nodes/node-1") == 0);

    char exact[30] = {0};
    assert(controller_build_node_topic(exact, sizeof(exact), "root", "node"));
    assert(strcmp(exact, "root/nodes/node") == 0);

    char too_small[15] = {0};
    assert(!controller_build_node_topic(too_small, sizeof(too_small), "root", "node"));
    assert(!controller_build_node_topic(NULL, 1, "root", "node"));
    assert(!controller_build_node_topic(topic, 0, "root", "node"));
    assert(!controller_build_node_topic(topic, sizeof(topic), "bad/root", "node"));
    assert(!controller_build_node_topic(topic, sizeof(topic), "root", "bad/node"));
}

static void test_unsigned_fields(void)
{
    const char *cursor = "0";
    unsigned long value = 99;
    assert(controller_parse_uint_field(&cursor, 100, '\0', &value));
    assert(value == 0 && *cursor == '\0');

    cursor = "100,7";
    assert(controller_parse_uint_field(&cursor, 100, ',', &value));
    assert(value == 100 && strcmp(cursor, "7") == 0);

    cursor = "101";
    assert(!controller_parse_uint_field(&cursor, 100, '\0', &value));
    cursor = "-1";
    assert(!controller_parse_uint_field(&cursor, 100, '\0', &value));
    cursor = "+1";
    assert(!controller_parse_uint_field(&cursor, 100, '\0', &value));
    cursor = " 1";
    assert(!controller_parse_uint_field(&cursor, 100, '\0', &value));
    cursor = "1 ";
    assert(!controller_parse_uint_field(&cursor, 100, '\0', &value));
    cursor = "1.0";
    assert(!controller_parse_uint_field(&cursor, 100, '\0', &value));
    cursor = "";
    assert(!controller_parse_uint_field(&cursor, 100, '\0', &value));
    assert(!controller_parse_uint_field(NULL, 100, '\0', &value));
}

static void test_percentages(void)
{
    uint8_t value = 255;
    assert(controller_parse_percent("0", &value) && value == 0);
    assert(controller_parse_percent("001", &value) && value == 1);
    assert(controller_parse_percent("100", &value) && value == 100);
    assert(!controller_parse_percent("101", &value));
    assert(!controller_parse_percent("-1", &value));
    assert(!controller_parse_percent("+1", &value));
    assert(!controller_parse_percent(" 50", &value));
    assert(!controller_parse_percent("50 ", &value));
    assert(!controller_parse_percent("50%", &value));
    assert(!controller_parse_percent("", &value));
    assert(!controller_parse_percent(NULL, &value));
    assert(!controller_parse_percent("50", NULL));
}

static void test_throttle_mapping(void)
{
    assert(controller_effective_pct(0, 15) == 0);
    assert(controller_effective_pct(1, 15) == 15);
    assert(controller_effective_pct(15, 15) == 15);
    assert(controller_effective_pct(80, 15) == 80);
    assert(controller_effective_pct(101, 200) == 100);

    assert(controller_pct_to_dshot(0, 15) == 0);
    assert(controller_pct_to_dshot(100, 15) == 2047);
    uint16_t previous = 0;
    for (unsigned int pct = 1; pct <= 100; ++pct) {
        uint16_t value = controller_pct_to_dshot((uint8_t)pct, 1);
        assert(value >= 48 && value <= 2047);
        assert(value >= previous);
        previous = value;
    }
    for (unsigned int pct = 1; pct <= 100; ++pct) {
        uint16_t value = controller_pct_to_dshot((uint8_t)pct, 15);
        assert(value == 0 || value >= 48);
        assert(value > 47);
    }
}

static void test_https_policy(void)
{
    const char *prefix = "https://updates.example.invalid/greenhouse/";
    assert(controller_https_url_allowed(
        "https://updates.example.invalid/greenhouse/fw.bin", prefix));
    assert(controller_https_url_allowed(prefix, prefix));
    assert(!controller_https_url_allowed(
        "http://updates.example.invalid/greenhouse/fw.bin", prefix));
    assert(!controller_https_url_allowed(
        "https://updates.example.invalid.evil/greenhouse/fw.bin", prefix));
    assert(!controller_https_url_allowed(
        "https://updates.example.invalid/other/fw.bin", prefix));
    assert(!controller_https_url_allowed(
        "https://updates.example.invalid/greenhouse/fw bin", prefix));
    assert(!controller_https_url_allowed("https://example/a", "https://example"));
    assert(!controller_https_url_allowed("https://example/a", "http://example/"));
    assert(!controller_https_url_allowed(NULL, prefix));
    assert(!controller_https_url_allowed("https://example/a", NULL));
}

static void test_dshot_frames(void)
{
    uint8_t bytes[2] = {0xff, 0xff};
    assert(dshot_protocol_build_frame(0, false, bytes));
    assert(bytes[0] == 0x00 && bytes[1] == 0x00);
    assert(dshot_protocol_build_frame(1, false, bytes));
    assert(bytes[0] == 0x00 && bytes[1] == 0x22);
    assert(dshot_protocol_build_frame(48, false, bytes));
    assert(bytes[0] == 0x06 && bytes[1] == 0x06);
    assert(dshot_protocol_build_frame(2047, false, bytes));
    assert(bytes[0] == 0xff && bytes[1] == 0xee);
    assert(dshot_protocol_build_frame(2047, true, bytes));
    assert(bytes[0] == 0xff && bytes[1] == 0xff);

    for (unsigned int throttle = 0; throttle <= DSHOT_PROTOCOL_MAX_THROTTLE; ++throttle) {
        for (unsigned int telemetry = 0; telemetry <= 1; ++telemetry) {
            assert(dshot_protocol_build_frame((uint16_t)throttle, telemetry != 0, bytes));
            uint16_t frame = (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
            uint16_t payload = (uint16_t)(frame >> 4);
            uint16_t checksum = (uint16_t)(frame & 0x0fU);
            assert(payload == (uint16_t)((throttle << 1) | telemetry));
            assert(checksum == (uint16_t)((payload ^ (payload >> 4) ^
                                           (payload >> 8)) & 0x0fU));
        }
    }

    bytes[0] = 0xaa;
    bytes[1] = 0x55;
    assert(!dshot_protocol_build_frame(2048, true, bytes));
    assert(bytes[0] == 0 && bytes[1] == 0);
    assert(!dshot_protocol_build_frame(0, false, NULL));
}

int main(void)
{
    test_identifiers();
    test_node_topics();
    test_unsigned_fields();
    test_percentages();
    test_throttle_mapping();
    test_https_policy();
    test_dshot_frames();
    puts("controller_policy host tests passed");
    return 0;
}
