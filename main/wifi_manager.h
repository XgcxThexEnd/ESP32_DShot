#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool initialized;
    bool connected_to_ap;
    bool has_ip;
    bool degraded;
    uint32_t retry_count;
    uint32_t disconnect_count;
    int64_t last_ip_us;
} wifi_manager_snapshot_t;

/**
 * Initialize and start the singleton Wi-Fi station manager and SNTP client.
 *
 * This function is intended to be called once during application startup.
 * A second call returns ESP_OK after successful initialization, or
 * ESP_ERR_INVALID_STATE while another initialization is in progress. Failed
 * attempts unwind owned resources and may be retried.
 */
esp_err_t wifi_manager_init(void);

/** Wait up to timeout_ticks for the station to acquire an IP address. */
bool wifi_manager_wait_for_ip(TickType_t timeout_ticks);

/** Return whether the station currently owns an IP address. */
bool wifi_manager_has_ip(void);

/** Return whether wall time is plausible for TLS certificate validation. */
bool wifi_manager_system_time_valid_for_tls(void);

/**
 * Wait up to timeout_ticks for SNTP synchronization and validate wall time.
 * Returns ESP_ERR_INVALID_STATE when the manager/SNTP client is unavailable or
 * when SNTP reports completion without producing a plausible wall clock.
 */
esp_err_t wifi_manager_wait_for_time(TickType_t timeout_ticks);

/** Copy a coherent, point-in-time manager snapshot into out_snapshot. */
void wifi_manager_get_snapshot(wifi_manager_snapshot_t *out_snapshot);

#ifdef __cplusplus
}
#endif
