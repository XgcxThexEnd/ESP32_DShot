#pragma once

#include "esp_err.h"

/**
 * Download one authorized HTTPS image without following redirects. Network I/O
 * has a ten-second idle timeout and a five-minute overall deadline. Select the
 * next boot partition only after the complete image passes ESP-IDF validation.
 * The caller owns motion inhibition, singleton admission, and reboot/cleanup.
 */
esp_err_t ota_download_verified(const char *url);
