#include "dshot_protocol.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define DSHOT_PROTOCOL_IMPL_ATTR IRAM_ATTR
#else
#define DSHOT_PROTOCOL_IMPL_ATTR
#endif

DSHOT_PROTOCOL_IMPL_ATTR bool dshot_protocol_build_frame(uint16_t throttle, bool telemetry,
                                                         uint8_t frame_bytes[2])
{
    if (!frame_bytes) return false;
    if (throttle > DSHOT_PROTOCOL_MAX_THROTTLE) {
        frame_bytes[0] = 0;
        frame_bytes[1] = 0;
        return false;
    }

    uint16_t payload = (uint16_t)((throttle << 1) | (telemetry ? 1U : 0U));
    uint16_t checksum = (uint16_t)((payload ^ (payload >> 4) ^ (payload >> 8)) & 0x0FU);
    uint16_t frame = (uint16_t)((payload << 4) | checksum);
    frame_bytes[0] = (uint8_t)(frame >> 8);
    frame_bytes[1] = (uint8_t)frame;
    return true;
}
