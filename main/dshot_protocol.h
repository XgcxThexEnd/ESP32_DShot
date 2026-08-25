#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DSHOT_PROTOCOL_MAX_THROTTLE 2047U

/**
 * Build the two bytes transmitted on the DShot wire, most-significant byte
 * first. Returns false and writes a zero-throttle frame when throttle is out of
 * the 11-bit protocol range.
 */
bool dshot_protocol_build_frame(uint16_t throttle, bool telemetry,
                                uint8_t frame_bytes[2]);

#ifdef __cplusplus
}
#endif
