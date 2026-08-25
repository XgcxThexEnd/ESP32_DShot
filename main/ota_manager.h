#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Stop/inhibit every motor and any automatic source which could restart it.
 * This callback is synchronous and is always called before the OTA manager
 * creates or releases the download task. The bounded URL claim/copy has
 * already completed at that point.
 */
typedef bool (*ota_manager_prepare_fn)(void *callback_context);

/** Return true only when wall time is trusted for TLS certificate checks. */
typedef bool (*ota_manager_trusted_time_fn)(void *callback_context);

/**
 * Undo transient preparation state (for example, an explicit ramp pause) when
 * an update fails after prepare returned true. Safety inhibits and zero
 * targets may remain latched at the coordinator's discretion.
 */
typedef void (*ota_manager_abort_fn)(void *callback_context);

/**
 * Publish one of the bounded status literals documented below. The callback
 * is invoked outside the manager lock and must remain bounded/nonblocking.
 */
typedef void (*ota_manager_publish_status_fn)(
    void *callback_context, const char *status, bool retain);

typedef struct {
    ota_manager_prepare_fn prepare;
    ota_manager_trusted_time_fn trusted_time;
    ota_manager_abort_fn abort;
    ota_manager_publish_status_fn publish_status;
    void *callback_context;
} ota_manager_config_t;

typedef enum {
    OTA_MANAGER_START_ACCEPTED = 0,
    OTA_MANAGER_START_REJECTED_URL,
    OTA_MANAGER_START_TIME_NOT_READY,
    OTA_MANAGER_START_BUSY,
    OTA_MANAGER_START_PREPARE_FAILED,
    OTA_MANAGER_START_ALLOC_FAILED,
    OTA_MANAGER_START_TASK_FAILED,
    OTA_MANAGER_START_NOT_INITIALIZED,
    OTA_MANAGER_START_INVALID_ARGUMENT,
    OTA_MANAGER_START_DISABLED,
} ota_manager_start_result_t;

/**
 * Install the singleton callbacks. Call this once during startup. When OTA is
 * compiled out this is a successful no-op so callers can initialize modules
 * unconditionally.
 */
esp_err_t ota_manager_init(const ota_manager_config_t *config);

/**
 * Validate/copy the URL and claim the singleton OTA operation. This phase is
 * bounded and performs no network I/O or safety/NVS transaction, so a caller
 * may run it under an external command/session fence.
 *
 * The configured URL prefix is enforced here even when the command router has
 * already validated the node-scoped request. Statuses are exactly:
 * "rejected", "starting", "failed", and "downloaded_rebooting".
 */
ota_manager_start_result_t ota_manager_start(const char *url);

/**
 * Complete a claimed start after releasing any external command/session lock.
 * For an accepted claim this performs safety preparation, creates the OTA
 * task, publishes status, and releases the task to perform network I/O. Call
 * exactly once for every result returned by ota_manager_start(); the returned
 * value is the final start result after preparation/task creation.
 */
ota_manager_start_result_t ota_manager_complete_start(
    ota_manager_start_result_t result);

/** Validate a URL against CONFIG_OTA_ALLOWED_URL_PREFIX. */
bool ota_manager_url_allowed(const char *url);

/** True from a successful singleton claim until failure or reboot. */
bool ota_manager_is_in_progress(void);

#ifdef __cplusplus
}
#endif
