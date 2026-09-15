#include "sdkconfig.h"

#ifdef CONFIG_OTA_ENABLED

#include "ota_transport.h"

#include <errno.h>
#include <stdlib.h>

#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_transport_ssl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
    esp_transport_handle_t tls;
    int64_t deadline_us;
    int timeout_ms;
    bool expired;
} ota_transport_context_t;

static int64_t operation_deadline(ota_transport_context_t *context, int timeout_ms)
{
    int wait_ms = context->timeout_ms;
    if (timeout_ms >= 0 && timeout_ms < wait_ms) wait_ms = timeout_ms;
    int64_t deadline = esp_timer_get_time() + (int64_t)wait_ms * 1000;
    return deadline < context->deadline_us ? deadline : context->deadline_us;
}

/* Round down so no socket wait extends beyond the remaining time budget. */
static int remaining_ms(int64_t deadline_us)
{
    int64_t remaining = deadline_us - esp_timer_get_time();
    return remaining > 0 ? (int)(remaining / 1000) : 0;
}

bool ota_transport_expired(esp_transport_handle_t transport)
{
    ota_transport_context_t *context = esp_transport_get_context_data(transport);
    return context && context->expired;
}

static int timed_out(ota_transport_context_t *context)
{
    context->expired = true;
    errno = ETIMEDOUT;
    /* A terminal error, distinct from the transport's retryable zero result. */
    return ERR_TCP_TRANSPORT_CONNECTION_FAILED;
}

static int transport_connect(esp_transport_handle_t transport, const char *host,
                             int port, int timeout_ms)
{
    ota_transport_context_t *context = esp_transport_get_context_data(transport);
    int64_t deadline = operation_deadline(context, timeout_ms);
    int poll_ms;

    /* Async TLS leaves the socket nonblocking. Otherwise a TLS read could wait
     * using the timeout captured at connect, beyond a later shorter deadline.
     * Pass the whole connection budget on the first call: IDF 5.5 caches this
     * timeout and does not restore select() fd_sets after a connect timeout.
     * Short polling slices can therefore strand an otherwise healthy socket.
     * A select timeout now consumes the budget and terminates this attempt;
     * subsequent successful progress is nonblocking TLS handshake work.
     * The SDK's initial DNS lookup retains its own lwIP timeout. */
    while ((poll_ms = remaining_ms(deadline)) > 0) {
        int result = esp_transport_connect_async(context->tls, host, port, poll_ms);
        if (esp_timer_get_time() >= deadline) return timed_out(context);
        if (result > 0) return 0;
        if (result < 0) return result;
        vTaskDelay(1);
    }
    return timed_out(context);
}

static int transport_read(esp_transport_handle_t transport, char *buffer, int len,
                          int timeout_ms)
{
    ota_transport_context_t *context = esp_transport_get_context_data(transport);
    int64_t deadline = operation_deadline(context, timeout_ms);
    int wait_ms;
    while ((wait_ms = remaining_ms(deadline)) > 0) {
        int result = esp_transport_read(context->tls, buffer, len, wait_ms);
        if (esp_timer_get_time() >= deadline) return timed_out(context);
        if (result != ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT) return result;
        /* Nonblocking TLS can need another record after the socket was ready. */
        vTaskDelay(1);
    }
    return timed_out(context);
}

static int transport_write(esp_transport_handle_t transport, const char *buffer,
                           int len, int timeout_ms)
{
    ota_transport_context_t *context = esp_transport_get_context_data(transport);
    int64_t deadline = operation_deadline(context, timeout_ms);
    int wait_ms;
    while ((wait_ms = remaining_ms(deadline)) > 0) {
        int result = esp_transport_write(context->tls, buffer, len, wait_ms);
        if (esp_timer_get_time() >= deadline) return timed_out(context);
        if (result != 0 && result != ESP_TLS_ERR_SSL_WANT_READ &&
            result != ESP_TLS_ERR_SSL_WANT_WRITE) {
            return result;
        }
        vTaskDelay(1);
    }
    return timed_out(context);
}

static int transport_poll_read(esp_transport_handle_t transport, int timeout_ms)
{
    ota_transport_context_t *context = esp_transport_get_context_data(transport);
    int wait_ms = remaining_ms(operation_deadline(context, timeout_ms));
    if (esp_timer_get_time() >= context->deadline_us) return timed_out(context);
    int result = esp_transport_poll_read(context->tls, wait_ms);
    return esp_timer_get_time() >= context->deadline_us ? timed_out(context) : result;
}

static int transport_poll_write(esp_transport_handle_t transport, int timeout_ms)
{
    ota_transport_context_t *context = esp_transport_get_context_data(transport);
    int wait_ms = remaining_ms(operation_deadline(context, timeout_ms));
    if (esp_timer_get_time() >= context->deadline_us) return timed_out(context);
    int result = esp_transport_poll_write(context->tls, wait_ms);
    return esp_timer_get_time() >= context->deadline_us ? timed_out(context) : result;
}

static int transport_close(esp_transport_handle_t transport)
{
    ota_transport_context_t *context = esp_transport_get_context_data(transport);
    return esp_transport_close(context->tls);
}

static int transport_destroy(esp_transport_handle_t transport)
{
    ota_transport_context_t *context = esp_transport_get_context_data(transport);
    esp_transport_destroy(context->tls);
    free(context);
    return 0;
}

esp_transport_handle_t ota_transport_create(int64_t deadline_us, int timeout_ms)
{
    if (timeout_ms <= 0 || deadline_us <= esp_timer_get_time()) return NULL;
    ota_transport_context_t *context = calloc(1, sizeof(*context));
    if (!context) return NULL;
    esp_transport_handle_t transport = esp_transport_init();
    context->tls = esp_transport_ssl_init();
    if (!transport || !context->tls) {
        esp_transport_destroy(transport);
        esp_transport_destroy(context->tls);
        free(context);
        return NULL;
    }

    context->deadline_us = deadline_us;
    context->timeout_ms = timeout_ms;
    esp_transport_ssl_crt_bundle_attach(context->tls, esp_crt_bundle_attach);
    esp_transport_set_context_data(transport, context);
    esp_transport_set_default_port(transport, 443);
    esp_transport_set_func(transport, transport_connect, transport_read,
                           transport_write, transport_close, transport_poll_read,
                           transport_poll_write, transport_destroy);
    return transport;
}

#endif
