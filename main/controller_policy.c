#include "controller_policy.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

bool controller_identifier_valid(const char *value, bool allow_empty)
{
    if (!value) return false;
    if (*value == '\0') return allow_empty;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (!(isalnum(*p) || *p == '_' || *p == '-')) return false;
    }
    return true;
}

bool controller_topic_level_valid(const char *value)
{
    return controller_identifier_valid(value, false);
}

bool controller_build_node_topic(char *out, size_t out_size,
                                 const char *root, const char *node_id)
{
    if (!out || out_size == 0 || !controller_topic_level_valid(root) ||
        !controller_identifier_valid(node_id, false)) {
        return false;
    }
    int written = snprintf(out, out_size, "%s/nodes/%s", root, node_id);
    return written > 0 && (size_t)written < out_size;
}

bool controller_parse_uint_field(const char **cursor, unsigned long limit,
                                 char delimiter, unsigned long *out)
{
    if (!cursor || !*cursor || !out) return false;
    const unsigned char *p = (const unsigned char *)*cursor;
    if (!isdigit(*p)) return false;

    unsigned long value = 0;
    while (isdigit(*p)) {
        unsigned int digit = *p - '0';
        if (value > limit / 10UL ||
            (value == limit / 10UL && digit > limit % 10UL)) {
            return false;
        }
        value = value * 10UL + digit;
        ++p;
    }
    if (delimiter) {
        if (*p != (unsigned char)delimiter) return false;
        ++p;
    } else if (*p != '\0') {
        return false;
    }
    *cursor = (const char *)p;
    *out = value;
    return true;
}

bool controller_parse_percent(const char *text, uint8_t *out)
{
    const char *cursor = text;
    unsigned long value = 0;
    if (!text || !out || !controller_parse_uint_field(&cursor, 100, '\0', &value)) {
        return false;
    }
    *out = (uint8_t)value;
    return true;
}

uint8_t controller_effective_pct(uint8_t requested_pct, uint8_t minimum_spin_pct)
{
    if (requested_pct == 0) return 0;
    if (requested_pct > 100) requested_pct = 100;
    if (minimum_spin_pct > 100) minimum_spin_pct = 100;
    return requested_pct < minimum_spin_pct ? minimum_spin_pct : requested_pct;
}

uint16_t controller_pct_to_dshot(uint8_t requested_pct, uint8_t minimum_spin_pct)
{
    uint8_t effective = controller_effective_pct(requested_pct, minimum_spin_pct);
    if (effective == 0) return 0;
    return (uint16_t)(48U + ((uint32_t)effective * (2047U - 48U)) / 100U);
}

bool controller_https_url_allowed(const char *url, const char *trusted_prefix)
{
    static const char scheme[] = "https://";
    if (!url || !trusted_prefix) return false;
    size_t prefix_len = strlen(trusted_prefix);
    if (prefix_len <= sizeof(scheme) - 1 ||
        strncmp(trusted_prefix, scheme, sizeof(scheme) - 1) != 0 ||
        trusted_prefix[prefix_len - 1] != '/' ||
        strncmp(url, trusted_prefix, prefix_len) != 0) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)url; *p; ++p) {
        if (*p <= 0x20 || *p == 0x7f || *p == '\\') return false;
    }
    // Refuse path normalization ambiguity. OTA URLs are machine-generated and
    // do not need encoded path octets or RFC 3986 dot segments.
    const char *path = url + prefix_len;
    const char *segment = path;
    for (const char *p = path;; ++p) {
        if (*p == '%') return false;
        if (*p == '/' || *p == '\0' || *p == '?' || *p == '#') {
            size_t segment_len = (size_t)(p - segment);
            if ((segment_len == 1 && segment[0] == '.') ||
                (segment_len == 2 && segment[0] == '.' && segment[1] == '.')) {
                return false;
            }
            if (*p != '/') break;
            segment = p + 1;
        }
    }
    return true;
}
