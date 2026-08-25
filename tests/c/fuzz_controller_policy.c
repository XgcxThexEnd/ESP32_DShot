#include "controller_policy.h"
#include "dshot_protocol.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 4096) return 0;
    char *first = malloc(size + 1);
    char *second = malloc(size + 1);
    if (!first || !second) {
        free(first);
        free(second);
        return 0;
    }
    memcpy(first, data, size);
    first[size] = '\0';
    for (size_t i = 0; i < size; ++i) {
        second[i] = (char)data[size - i - 1];
    }
    second[size] = '\0';

    (void)controller_identifier_valid(first, data && size && (data[0] & 1U));
    (void)controller_topic_level_valid(first);

    char topic[96];
    (void)controller_build_node_topic(topic, sizeof(topic), first, second);
    size_t output_size = size ? (size_t)(data[0] % sizeof(topic)) : 0;
    (void)controller_build_node_topic(topic, output_size, first, second);

    const char *cursor = first;
    unsigned long parsed = 0;
    char delimiter = size > 1 ? (char)data[1] : '\0';
    (void)controller_parse_uint_field(&cursor, size ? data[0] : 0, delimiter, &parsed);

    uint8_t percent = 0;
    if (controller_parse_percent(first, &percent)) assert(percent <= 100);
    for (unsigned int minimum = 0; minimum <= 255; minimum += 17) {
        uint16_t dshot = controller_pct_to_dshot(percent, (uint8_t)minimum);
        assert(dshot == 0 || (dshot >= 48 && dshot <= 2047));
    }
    (void)controller_https_url_allowed(first, second);

    uint8_t frame[2] = {0};
    uint16_t throttle = size > 1 ? (uint16_t)(((uint16_t)data[0] << 8) | data[1]) : 0;
    bool frame_valid = dshot_protocol_build_frame(throttle, size > 2 && (data[2] & 1U), frame);
    if (frame_valid) {
        uint16_t encoded = (uint16_t)(((uint16_t)frame[0] << 8) | frame[1]);
        assert((encoded >> 5) == throttle);
    } else {
        assert(frame[0] == 0 && frame[1] == 0);
    }

    free(second);
    free(first);
    return 0;
}
