#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool controller_identifier_valid(const char *value, bool allow_empty);
bool controller_topic_level_valid(const char *value);
bool controller_build_node_topic(char *out, size_t out_size,
                                 const char *root, const char *node_id);
bool controller_parse_uint_field(const char **cursor, unsigned long limit,
                                 char delimiter, unsigned long *out);
bool controller_parse_percent(const char *text, uint8_t *out);
uint8_t controller_effective_pct(uint8_t requested_pct, uint8_t minimum_spin_pct);
uint16_t controller_pct_to_dshot(uint8_t requested_pct, uint8_t minimum_spin_pct);
bool controller_https_url_allowed(const char *url, const char *trusted_prefix);

#ifdef __cplusplus
}
#endif
