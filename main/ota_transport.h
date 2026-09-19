#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_transport.h"

/**
 * Create a verified TLS transport with an absolute esp_timer_get_time() deadline
 * and a maximum wait per I/O operation. Use with a blocking HTTP client.
 *
 * The caller owns this transport. Clean up the HTTP client before calling
 * esp_transport_destroy() on the returned handle. The custom transport and its
 * underlying TLS transport are not owned by the HTTP client's transport list.
 * All calls must remain on the owning download task.
 */
esp_transport_handle_t ota_transport_create(int64_t deadline_us, int timeout_ms);

/** True if an operation failed because its I/O or total deadline expired. */
bool ota_transport_expired(esp_transport_handle_t transport);
