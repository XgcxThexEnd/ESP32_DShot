#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MQTT_MANAGER_TOPIC_CAPACITY   192
#define MQTT_MANAGER_PAYLOAD_CAPACITY 768

/**
 * Identifies the broker connection which delivered one assembled command.
 * Treat this value as opaque and pass it only to the commit-fence API below.
 */
typedef struct {
    uint32_t connection_generation;
} mqtt_manager_command_session_t;

/**
 * Called in FIFO order from the manager's dedicated command-dispatch task
 * after all fragments of a command have been validated and assembled. Queued
 * work from a disconnected/replaced broker session is discarded.
 *
 * topic and payload are NUL-terminated, but payload_length remains the
 * authoritative payload size. Both buffers are owned by the dispatch task and
 * are valid only until this callback returns. The callback must copy anything
 * it needs to retain and should remain bounded so the command queue cannot be
 * held indefinitely. Before committing any command mutation, revalidate the
 * supplied session with mqtt_manager_command_commit_begin().
 */
typedef void (*mqtt_manager_message_callback_t)(
    void *context,
    const char *topic,
    const char *payload,
    size_t payload_length,
    bool retained,
    const mqtt_manager_command_session_t *session);

/** A point-in-time copy; callers cannot mutate manager-owned health state. */
typedef struct {
    bool connected;
    bool ready;
    /** PUBACK/SUBACK proof (or initial startup grace baseline), never RX. */
    int64_t last_ack_us;
    /** Last received command fragment, including rejected input; diagnostic only. */
    int64_t last_rx_us;
    int64_t connected_since_us;
    uint32_t puback_count;
    uint32_t publish_failures;
    uint32_t suback_count;
    uint32_t connect_count;
    /** Commands whose callback completed on the dispatch task. */
    uint32_t command_dispatch_count;
    bool command_dispatch_ready;
    bool command_in_flight;
    int64_t command_dispatch_heartbeat_us;
    /** Oldest queued or in-flight command; zero when the dispatcher is idle. */
    int64_t oldest_command_since_us;
    uint32_t command_dispatch_stack_bytes;
    /** Queue saturation events; each forces a fail-closed reconnect. */
    uint32_t command_queue_drops;
    /** Commands discarded during enqueue/dispatch because their session aged. */
    uint32_t stale_command_drops;
    /** Mutex/queue/task allocation failures observed during init attempts. */
    uint32_t dispatch_allocation_failures;
    size_t pending_subscription_count;
    size_t pending_command_count;
    /** Current MQTT outbox use, or -1 while lifecycle inspection is busy. */
    int outbox_bytes;
} mqtt_manager_health_t;

/**
 * Initialize the singleton manager after app_config_init() succeeds.
 * This creates the bounded command queue and its dispatch task, but does not
 * allocate or start an MQTT client. A failed initialization is fully unwound
 * and may be retried.
 */
esp_err_t mqtt_manager_init(mqtt_manager_message_callback_t message_callback,
                            void *callback_context);

/**
 * Revalidate a dispatched command at its application commit point and acquire
 * the session fence. A successful call linearizes the following mutation
 * before any disconnect, reconnect, or fail-closed queue invalidation.
 *
 * Call only from the command-dispatch callback. Keep the critical section
 * bounded, do not publish through MQTT while holding it, and pair every
 * successful begin with mqtt_manager_command_commit_end().
 */
esp_err_t mqtt_manager_command_commit_begin(
    const mqtt_manager_command_session_t *session, TickType_t timeout_ticks);
void mqtt_manager_command_commit_end(void);

/**
 * Create and start the ESP-MQTT client. Repeated calls are harmless while its
 * task is active; a client whose task terminated is rebuilt after a short
 * teardown grace period. An mqtts:// connection is deferred until wall time is
 * plausible so certificate date validation cannot be bypassed.
 */
esp_err_t mqtt_manager_start(void);

/**
 * Publish through the managed client. Transport connection is required, but
 * command-plane readiness is intentionally not required (startup metadata and
 * health probes may be published while subscriptions are settling).
 */
bool mqtt_manager_publish(const char *topic, const char *payload,
                          int qos, bool retain);

/**
 * Process a requested/unhealthy-subscription disconnect. Call regularly from
 * a normal application task, never from a motor-control or TWDT-critical task;
 * this function may briefly wait while publishing the offline LWT state.
 */
void mqtt_manager_poll(void);

bool mqtt_manager_connected(void);
bool mqtt_manager_ready(void);

/** Atomically consume the one-shot request for initial discovery/state output. */
bool mqtt_manager_take_initial_publish(void);

/**
 * Start/reset the local acknowledgement-age baseline. This preserves the cold
 * boot communication-lease/watchdog behavior before the first broker ACK.
 */
void mqtt_manager_begin_ack_window(void);

mqtt_manager_health_t mqtt_manager_health_snapshot(void);

#ifdef __cplusplus
}
#endif
