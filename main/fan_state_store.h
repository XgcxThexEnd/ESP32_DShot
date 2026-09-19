#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_config.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*fan_state_store_snapshot_fn)(void *context, uint8_t *targets,
                                            size_t target_count);
typedef bool (*fan_state_store_restore_fn)(void *context, const uint8_t *targets,
                                           size_t target_count);

typedef struct {
    const app_config_t *app_config;
    fan_state_store_snapshot_fn snapshot_targets;
    fan_state_store_restore_fn restore_targets;
    void *callback_context;
} fan_state_store_config_t;

typedef struct {
    bool initialized;
    bool save_task_ready;
    bool erased_on_boot;
    bool erase_observed;
    uint32_t save_task_stack_bytes;
} fan_state_store_health_t;

/** Initialize the store after NVS and app_config are ready. */
esp_err_t fan_state_store_init(const fan_state_store_config_t *config,
                               bool erased_on_boot);

/** Start the debounced writer task when restoration is enabled. */
esp_err_t fan_state_store_start(void);

/**
 * Load a topology-matched target blob. Legacy blobs are migrated only when
 * every stored target is provably zero.
 */
void fan_state_store_restore(void);

/** Coalesce a future save request; a no-op when restoration is disabled. */
void fan_state_store_request_save(void);

/**
 * Persist the latest target snapshot and wait for the NVS commit to finish.
 *
 * The operation is serialized with the debounced writer and must be called
 * from task context. timeout_ticks bounds both admission to the synchronous
 * request path and the completion wait. A timeout does not cancel a commit
 * that the writer has already accepted.
 *
 * Returns ESP_OK immediately when fan-state restoration is disabled, because
 * there is no persistent fan state to update. Otherwise returns the NVS error,
 * ESP_ERR_TIMEOUT, or ESP_ERR_INVALID_STATE when the store task is unavailable.
 */
esp_err_t fan_state_store_save_sync(TickType_t timeout_ticks);

fan_state_store_health_t fan_state_store_health_snapshot(void);

#ifdef __cplusplus
}
#endif
