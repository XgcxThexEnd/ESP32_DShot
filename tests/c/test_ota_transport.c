/* Exercise the production deadline transport through its public callbacks. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ota_transport.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_transport_ssl.h"
#include "freertos/task.h"

struct esp_transport_item_t {
    bool tls;
    int port;
    void *context;
    connect_func connect;
    io_read_func read;
    io_func write;
    trans_func close;
    poll_func poll_read;
    poll_func poll_write;
    trans_func destroy;
};

enum operation { CONNECT, READ, WRITE, POLL_READ, POLL_WRITE, OPERATION_COUNT };
enum { MAX_STEPS = 32 };
typedef struct {
    int result;
    int64_t elapsed_us;
} response_t;
typedef struct {
    response_t responses[MAX_STEPS];
    unsigned response_count;
    unsigned calls;
    int timeouts[MAX_STEPS];
} operation_t;

static operation_t operations[OPERATION_COUNT];
static int64_t now_us;
static unsigned live_handles;
static unsigned tls_destroy_count;
static unsigned wrapper_destroy_count;
static unsigned close_count;
static unsigned bundle_attach_count;
static unsigned delay_count;
static bool fail_wrapper_init;
static bool fail_tls_init;
static bool emulate_sdk_connect_timeout;
static int cached_connect_timeout_ms;
static bool connect_fdsets_cleared;

static void reset_fixture(void)
{
    assert(live_handles == 0);
    memset(operations, 0, sizeof(operations));
    now_us = 0;
    tls_destroy_count = 0;
    wrapper_destroy_count = 0;
    close_count = 0;
    bundle_attach_count = 0;
    delay_count = 0;
    fail_wrapper_init = false;
    fail_tls_init = false;
    emulate_sdk_connect_timeout = false;
    cached_connect_timeout_ms = 0;
    connect_fdsets_cleared = false;
    errno = 0;
}

static void respond(enum operation operation, int result, int64_t elapsed_us)
{
    operation_t *plan = &operations[operation];
    assert(plan->response_count < MAX_STEPS);
    plan->responses[plan->response_count++] = (response_t){result, elapsed_us};
}

static int perform(enum operation operation, int timeout_ms)
{
    operation_t *plan = &operations[operation];
    assert(plan->calls < plan->response_count);
    assert(timeout_ms >= 0);
    unsigned call = plan->calls++;
    plan->timeouts[call] = timeout_ms;
    now_us += plan->responses[call].elapsed_us;
    return plan->responses[call].result;
}

int64_t esp_timer_get_time(void) { return now_us; }

void vTaskDelay(TickType_t ticks)
{
    assert(ticks == 1);
    ++delay_count;
    now_us += (int64_t)ticks * 1000;
}

esp_err_t esp_crt_bundle_attach(void *conf)
{
    (void)conf;
    return ESP_OK;
}

static esp_transport_handle_t allocate_transport(bool tls)
{
    esp_transport_handle_t transport = calloc(1, sizeof(*transport));
    assert(transport);
    transport->tls = tls;
    ++live_handles;
    return transport;
}

esp_transport_handle_t esp_transport_init(void)
{
    return fail_wrapper_init ? NULL : allocate_transport(false);
}

esp_transport_handle_t esp_transport_ssl_init(void)
{
    return fail_tls_init ? NULL : allocate_transport(true);
}

esp_err_t esp_transport_destroy(esp_transport_handle_t transport)
{
    if (!transport) return ESP_OK;
    if (transport->tls) {
        ++tls_destroy_count;
    } else {
        ++wrapper_destroy_count;
        if (transport->destroy) assert(transport->destroy(transport) == 0);
    }
    assert(live_handles > 0);
    --live_handles;
    free(transport);
    return ESP_OK;
}

int esp_transport_get_default_port(esp_transport_handle_t transport)
{
    return transport->port;
}

esp_err_t esp_transport_set_default_port(esp_transport_handle_t transport, int port)
{
    transport->port = port;
    return ESP_OK;
}

void *esp_transport_get_context_data(esp_transport_handle_t transport)
{
    return transport ? transport->context : NULL;
}

esp_err_t esp_transport_set_context_data(esp_transport_handle_t transport, void *data)
{
    transport->context = data;
    return ESP_OK;
}

esp_err_t esp_transport_set_func(esp_transport_handle_t transport,
                               connect_func connect, io_read_func read,
                               io_func write, trans_func close,
                               poll_func poll_read, poll_func poll_write,
                               trans_func destroy)
{
    transport->connect = connect;
    transport->read = read;
    transport->write = write;
    transport->close = close;
    transport->poll_read = poll_read;
    transport->poll_write = poll_write;
    transport->destroy = destroy;
    return ESP_OK;
}

void esp_transport_ssl_crt_bundle_attach(esp_transport_handle_t transport,
                                        esp_err_t (*attach)(void *))
{
    assert(transport->tls);
    assert(attach == esp_crt_bundle_attach);
    ++bundle_attach_count;
}

int esp_transport_connect(esp_transport_handle_t transport, const char *host,
                          int port, int timeout_ms)
{
    assert(!transport->tls); /* TLS must be connected asynchronously. */
    return transport->connect(transport, host, port, timeout_ms);
}

int esp_transport_connect_async(esp_transport_handle_t transport, const char *host,
                                int port, int timeout_ms)
{
    assert(transport->tls);
    assert(strcmp(host, "updates.example.invalid") == 0);
    assert(port == 443);
    if (emulate_sdk_connect_timeout) {
        /* Model IDF's cached timeout and select's emptied fd_sets. The TCP
         * connection becomes writable at 100 ms, beyond the old 50 ms slice. */
        if (!cached_connect_timeout_ms) cached_connect_timeout_ms = timeout_ms;
        if (!connect_fdsets_cleared && cached_connect_timeout_ms > 100) {
            now_us += 100000;
            return 1;
        }
        now_us += (int64_t)cached_connect_timeout_ms * 1000;
        connect_fdsets_cleared = true;
        return 0;
    }
    return perform(CONNECT, timeout_ms);
}

int esp_transport_read(esp_transport_handle_t transport, char *buffer, int len,
                       int timeout_ms)
{
    if (!transport->tls) return transport->read(transport, buffer, len, timeout_ms);
    int result = perform(READ, timeout_ms);
    if (result > 0) {
        assert(result <= len);
        memset(buffer, 'a', (size_t)result);
    }
    return result;
}

int esp_transport_write(esp_transport_handle_t transport, const char *buffer,
                        int len, int timeout_ms)
{
    if (!transport->tls) return transport->write(transport, buffer, len, timeout_ms);
    assert(buffer && len > 0);
    return perform(WRITE, timeout_ms);
}

int esp_transport_poll_read(esp_transport_handle_t transport, int timeout_ms)
{
    return transport->tls ? perform(POLL_READ, timeout_ms)
                          : transport->poll_read(transport, timeout_ms);
}

int esp_transport_poll_write(esp_transport_handle_t transport, int timeout_ms)
{
    return transport->tls ? perform(POLL_WRITE, timeout_ms)
                          : transport->poll_write(transport, timeout_ms);
}

int esp_transport_close(esp_transport_handle_t transport)
{
    if (!transport->tls) return transport->close(transport);
    ++close_count;
    return 0;
}

static void expect_timeout(esp_transport_handle_t transport, int result)
{
    assert(result == ERR_TCP_TRANSPORT_CONNECTION_FAILED);
    assert(result != ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT);
    assert(errno == ETIMEDOUT);
    assert(ota_transport_expired(transport));
}

static void test_verified_transport_ownership(void)
{
    reset_fixture();
    esp_transport_handle_t transport = ota_transport_create(1000000, 100);
    assert(transport && live_handles == 2);
    assert(bundle_attach_count == 1);
    assert(esp_transport_get_default_port(transport) == 443);
    assert(esp_transport_close(transport) == 0);
    assert(close_count == 1 && live_handles == 2);
    assert(esp_transport_destroy(transport) == ESP_OK);
    assert(live_handles == 0);
    assert(tls_destroy_count == 1 && wrapper_destroy_count == 1);
}

static void test_creation_failures_release_owned_transports(void)
{
    reset_fixture();
    assert(!ota_transport_create(0, 10));
    assert(!ota_transport_create(1000, 0));
    assert(!ota_transport_create(1000, -1));
    assert(live_handles == 0);
    fail_wrapper_init = true;
    assert(!ota_transport_create(1000000, 100));
    assert(live_handles == 0 && tls_destroy_count == 1);
    reset_fixture();
    fail_tls_init = true;
    assert(!ota_transport_create(1000000, 100));
    assert(live_handles == 0 && wrapper_destroy_count == 1);
}

static void test_read_wait_uses_smallest_budget(void)
{
    reset_fixture();
    esp_transport_handle_t transport = ota_transport_create(500000, 200);
    char buffer[8];
    respond(READ, 2, 0);
    respond(READ, 2, 0);
    respond(READ, 2, 0);
    assert(esp_transport_read(transport, buffer, sizeof(buffer), 40) == 2);
    assert(operations[READ].timeouts[0] == 40);
    assert(esp_transport_read(transport, buffer, sizeof(buffer), -1) == 2);
    assert(operations[READ].timeouts[1] == 200);
    now_us = 475500;
    assert(esp_transport_read(transport, buffer, sizeof(buffer), 1000) == 2);
    assert(operations[READ].timeouts[2] == 24);
    assert(!ota_transport_expired(transport));
    esp_transport_destroy(transport);
}

static void test_slow_partial_reads_cannot_renew_total_deadline(void)
{
    reset_fixture();
    esp_transport_handle_t transport = ota_transport_create(1000000, 500);
    char buffer[8];
    respond(READ, 1, 400000);
    respond(READ, 1, 400000);
    respond(READ, 1, 200000);
    assert(esp_transport_read(transport, buffer, sizeof(buffer), 500) == 1);
    assert(esp_transport_read(transport, buffer, sizeof(buffer), 500) == 1);
    expect_timeout(transport, esp_transport_read(transport, buffer, sizeof(buffer), 500));
    assert(operations[READ].calls == 3);
    assert(operations[READ].timeouts[0] == 500);
    assert(operations[READ].timeouts[1] == 500);
    assert(operations[READ].timeouts[2] == 200);
    /* A header parser retry after expiration must not touch the socket again. */
    expect_timeout(transport, esp_transport_read(transport, buffer, sizeof(buffer), 500));
    assert(operations[READ].calls == 3);
    esp_transport_destroy(transport);
}

static void test_read_retries_share_an_operation_budget(void)
{
    reset_fixture();
    esp_transport_handle_t transport = ota_transport_create(1000000, 5);
    char buffer[8];
    respond(READ, 0, 1000);
    respond(READ, 0, 1000);
    respond(READ, 0, 1000);
    expect_timeout(transport, esp_transport_read(transport, buffer, sizeof(buffer), 100));
    assert(now_us == 5000 && delay_count == 2);
    assert(operations[READ].calls == 3);
    assert(operations[READ].timeouts[0] == 5);
    assert(operations[READ].timeouts[1] == 3);
    assert(operations[READ].timeouts[2] == 1);
    esp_transport_destroy(transport);
}

static void test_read_retry_can_succeed_within_budget(void)
{
    reset_fixture();
    esp_transport_handle_t transport = ota_transport_create(1000000, 10);
    char buffer[8];
    respond(READ, 0, 2000);
    respond(READ, 2, 1000);
    assert(esp_transport_read(transport, buffer, sizeof(buffer), 10) == 2);
    assert(delay_count == 1 && now_us == 4000);
    assert(operations[READ].timeouts[1] == 7);
    assert(!ota_transport_expired(transport));
    esp_transport_destroy(transport);
}

static void test_connect_async_progress_and_completion(void)
{
    reset_fixture();
    esp_transport_handle_t transport = ota_transport_create(1000000, 100);
    respond(CONNECT, 0, 20000);
    respond(CONNECT, 1, 10000);
    assert(esp_transport_connect(transport, "updates.example.invalid", 443, 100) == 0);
    assert(delay_count == 1 && now_us == 31000);
    assert(operations[CONNECT].calls == 2);
    assert(operations[CONNECT].timeouts[0] == 100);
    assert(!ota_transport_expired(transport));
    esp_transport_destroy(transport);
}

static void test_connect_survives_latency_above_old_poll_slice(void)
{
    reset_fixture();
    emulate_sdk_connect_timeout = true;
    esp_transport_handle_t transport = ota_transport_create(1000000, 500);
    assert(esp_transport_connect(transport, "updates.example.invalid", 443, 500) == 0);
    assert(now_us == 100000 && !connect_fdsets_cleared);
    assert(!ota_transport_expired(transport));
    esp_transport_destroy(transport);
}

static void test_connect_async_wait_expires(void)
{
    reset_fixture();
    esp_transport_handle_t transport = ota_transport_create(5000, 100);
    respond(CONNECT, 0, 1000);
    respond(CONNECT, 0, 1000);
    respond(CONNECT, 0, 1000);
    expect_timeout(transport, esp_transport_connect(transport, "updates.example.invalid", 443, 100));
    assert(now_us == 5000 && delay_count == 2);
    assert(operations[CONNECT].timeouts[0] == 5);
    assert(operations[CONNECT].timeouts[1] == 3);
    assert(operations[CONNECT].timeouts[2] == 1);
    esp_transport_destroy(transport);
}

static void test_connect_late_completion_fails(void)
{
    reset_fixture();
    esp_transport_handle_t transport = ota_transport_create(1000000, 5);
    respond(CONNECT, 1, 5000);
    expect_timeout(transport, esp_transport_connect(transport, "updates.example.invalid", 443, 100));
    assert(delay_count == 0);
    esp_transport_destroy(transport);
}

static void test_write_tls_retries_share_deadline(void)
{
    reset_fixture();
    esp_transport_handle_t transport = ota_transport_create(5000, 100);
    respond(WRITE, ESP_TLS_ERR_SSL_WANT_WRITE, 1000);
    respond(WRITE, ESP_TLS_ERR_SSL_WANT_READ, 1000);
    respond(WRITE, 0, 1000);
    expect_timeout(transport, esp_transport_write(transport, "header", 6, -1));
    assert(now_us == 5000 && delay_count == 2);
    assert(operations[WRITE].timeouts[0] == 5);
    assert(operations[WRITE].timeouts[1] == 3);
    assert(operations[WRITE].timeouts[2] == 1);
    esp_transport_destroy(transport);
}

static void test_write_progress_and_terminal_errors(void)
{
    reset_fixture();
    esp_transport_handle_t transport = ota_transport_create(1000000, 20);
    respond(WRITE, 0, 1000);
    respond(WRITE, 3, 1000);
    respond(WRITE, -7, 0);
    assert(esp_transport_write(transport, "header", 6, 10) == 3);
    assert(operations[WRITE].timeouts[0] == 10);
    assert(operations[WRITE].timeouts[1] == 8);
    assert(esp_transport_write(transport, "der", 3, 10) == -7);
    assert(!ota_transport_expired(transport));
    esp_transport_destroy(transport);
}

static void test_poll_waits_are_clamped_and_total_expiry_is_terminal(void)
{
    reset_fixture();
    esp_transport_handle_t transport = ota_transport_create(100000, 50);
    respond(POLL_READ, 1, 0);
    respond(POLL_WRITE, 1, 0);
    respond(POLL_WRITE, 1, 10000);
    assert(esp_transport_poll_read(transport, 10) == 1);
    assert(operations[POLL_READ].timeouts[0] == 10);
    assert(esp_transport_poll_write(transport, -1) == 1);
    assert(operations[POLL_WRITE].timeouts[0] == 50);
    now_us = 90000;
    expect_timeout(transport, esp_transport_poll_write(transport, 1000));
    assert(operations[POLL_WRITE].timeouts[1] == 10);
    expect_timeout(transport, esp_transport_poll_read(transport, 1000));
    assert(operations[POLL_READ].calls == 1);
    esp_transport_destroy(transport);
}

static void test_expiry_query_preserves_completed_transfer_result(void)
{
    reset_fixture();
    esp_transport_handle_t transport = ota_transport_create(10000, 10);
    char buffer[8];
    respond(READ, 1, 9000);
    assert(esp_transport_read(transport, buffer, sizeof(buffer), 10) == 1);
    now_us = 20000; /* Flash finalization/cleanup can consume time after EOF. */
    assert(!ota_transport_expired(transport));
    esp_transport_close(transport);
    assert(!ota_transport_expired(transport));
    esp_transport_destroy(transport);
}

static void test_zero_and_submillisecond_waits_fail_without_io(void)
{
    reset_fixture();
    esp_transport_handle_t transport = ota_transport_create(10000, 10);
    char buffer[8];
    expect_timeout(transport, esp_transport_read(transport, buffer, sizeof(buffer), 0));
    assert(operations[READ].calls == 0);
    esp_transport_destroy(transport);
    reset_fixture();
    transport = ota_transport_create(10000, 10);
    now_us = 9500;
    expect_timeout(transport, esp_transport_write(transport, "x", 1, 10));
    assert(operations[WRITE].calls == 0);
    esp_transport_destroy(transport);
}

static void test_connection_and_read_errors_are_preserved(void)
{
    reset_fixture();
    esp_transport_handle_t transport = ota_transport_create(1000000, 10);
    char buffer[8];
    respond(CONNECT, -7, 0);
    respond(READ, ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN, 0);
    assert(esp_transport_connect(transport, "updates.example.invalid", 443, 10) == -7);
    assert(esp_transport_read(transport, buffer, sizeof(buffer), 10) == ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN);
    assert(!ota_transport_expired(transport));
    esp_transport_destroy(transport);
}

int main(void)
{
    test_verified_transport_ownership();
    test_creation_failures_release_owned_transports();
    test_read_wait_uses_smallest_budget();
    test_slow_partial_reads_cannot_renew_total_deadline();
    test_read_retries_share_an_operation_budget();
    test_read_retry_can_succeed_within_budget();
    test_connect_async_progress_and_completion();
    test_connect_survives_latency_above_old_poll_slice();
    test_connect_async_wait_expires();
    test_connect_late_completion_fails();
    test_write_tls_retries_share_deadline();
    test_write_progress_and_terminal_errors();
    test_poll_waits_are_clamped_and_total_expiry_is_terminal();
    test_expiry_query_preserves_completed_transfer_result();
    test_zero_and_submillisecond_waits_fail_without_io();
    test_connection_and_read_errors_are_preserved();
    assert(live_handles == 0);
    puts("ota_transport: 16 tests passed");
    return 0;
}
