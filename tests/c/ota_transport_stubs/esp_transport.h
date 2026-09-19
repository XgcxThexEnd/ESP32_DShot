#pragma once
#include "esp_err.h"

typedef struct esp_transport_item_t *esp_transport_handle_t;
typedef int (*connect_func)(esp_transport_handle_t, const char *, int, int);
typedef int (*io_func)(esp_transport_handle_t, const char *, int, int);
typedef int (*io_read_func)(esp_transport_handle_t, char *, int, int);
typedef int (*trans_func)(esp_transport_handle_t);
typedef int (*poll_func)(esp_transport_handle_t, int);

enum esp_tcp_transport_err_t {
    ERR_TCP_TRANSPORT_NO_MEM = -3,
    ERR_TCP_TRANSPORT_CONNECTION_FAILED = -2,
    ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN = -1,
    ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT = 0,
};

esp_transport_handle_t esp_transport_init(void);
esp_err_t esp_transport_destroy(esp_transport_handle_t transport);
int esp_transport_get_default_port(esp_transport_handle_t transport);
esp_err_t esp_transport_set_default_port(esp_transport_handle_t transport, int port);
int esp_transport_connect(esp_transport_handle_t transport, const char *host,
                          int port, int timeout_ms);
int esp_transport_connect_async(esp_transport_handle_t transport, const char *host,
                                int port, int timeout_ms);
int esp_transport_read(esp_transport_handle_t transport, char *buffer, int len,
                       int timeout_ms);
int esp_transport_write(esp_transport_handle_t transport, const char *buffer,
                        int len, int timeout_ms);
int esp_transport_poll_read(esp_transport_handle_t transport, int timeout_ms);
int esp_transport_poll_write(esp_transport_handle_t transport, int timeout_ms);
int esp_transport_close(esp_transport_handle_t transport);
void *esp_transport_get_context_data(esp_transport_handle_t transport);
esp_err_t esp_transport_set_context_data(esp_transport_handle_t transport, void *data);
esp_err_t esp_transport_set_func(esp_transport_handle_t transport,
                               connect_func connect, io_read_func read,
                               io_func write, trans_func close,
                               poll_func poll_read, poll_func poll_write,
                               trans_func destroy);
