#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "ota_download.h"
#include "ota_transport.h"

struct fake_http_client { int unused; };
struct fake_transport { bool expired; };
static struct fake_http_client client;
static struct fake_transport transport;
static esp_partition_t partition;
static int status_code;
static int64_t content_length, now_us, deadline_us;
static int open_count, cleanup_count, destroy_count;
static int begin_count, end_count, abort_count, boot_count;
static esp_err_t open_result, begin_result, write_result, end_result, boot_result;
static bool complete, partition_present;
static size_t total_written, read_index, read_count;
static struct { int bytes; int64_t elapsed_us; } reads[8];
static const char *const url = CONFIG_OTA_ALLOWED_URL_PREFIX "image.bin";

static void reset(void)
{
    memset(&transport, 0, sizeof(transport));
    partition.size = 8192;
    status_code = 200;
    content_length = 8;
    now_us = 1000000;
    deadline_us = 0;
    open_count = cleanup_count = destroy_count = 0;
    begin_count = end_count = abort_count = boot_count = 0;
    open_result = begin_result = write_result = end_result = boot_result = ESP_OK;
    complete = partition_present = true;
    total_written = read_index = 0;
    read_count = 2;
    memset(reads, 0, sizeof(reads));
    reads[0].bytes = 8;
}

esp_err_t esp_crt_bundle_attach(void *config)
{
    (void)config;
    return ESP_OK;
}

int64_t esp_timer_get_time(void) { return now_us; }

esp_transport_handle_t ota_transport_create(int64_t deadline, int timeout_ms)
{
    assert(deadline == now_us + INT64_C(300000000));
    assert(timeout_ms == 10000);
    deadline_us = deadline;
    return &transport;
}

bool ota_transport_expired(esp_transport_handle_t handle)
{
    assert(handle == &transport);
    return transport.expired;
}

esp_err_t esp_transport_destroy(esp_transport_handle_t handle)
{
    assert(handle == &transport);
    assert(cleanup_count == 1);
    ++destroy_count;
    return ESP_OK;
}

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    assert(strcmp(config->url, url) == 0);
    assert(config->disable_auto_redirect);
    assert(config->crt_bundle_attach == esp_crt_bundle_attach);
    assert(config->transport == &transport);
    return &client;
}

esp_err_t esp_http_client_open(esp_http_client_handle_t handle, int write_len)
{
    assert(handle == &client && write_len == 0);
    ++open_count;
    return open_result;
}

int64_t esp_http_client_fetch_headers(esp_http_client_handle_t handle)
{
    assert(handle == &client);
    return content_length;
}

int esp_http_client_get_status_code(esp_http_client_handle_t handle)
{
    assert(handle == &client);
    return status_code;
}

int esp_http_client_read(esp_http_client_handle_t handle, char *buffer, int length)
{
    assert(handle == &client && read_index < read_count);
    int bytes = reads[read_index].bytes;
    now_us += reads[read_index++].elapsed_us;
    assert(bytes <= length);
    if (bytes > 0) memset(buffer, 0xe9, (size_t)bytes);
    return bytes;
}

bool esp_http_client_is_complete_data_received(esp_http_client_handle_t handle)
{
    assert(handle == &client);
    return complete;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t handle)
{
    assert(handle == &client);
    ++cleanup_count;
    return ESP_OK;
}

const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *start)
{
    assert(start == NULL);
    return partition_present ? &partition : NULL;
}

esp_err_t esp_ota_begin(const esp_partition_t *part, size_t size, esp_ota_handle_t *handle)
{
    assert(part == &partition && size == OTA_SIZE_UNKNOWN);
    ++begin_count;
    *handle = 42;
    return begin_result;
}

esp_err_t esp_ota_write(esp_ota_handle_t handle, const void *data, size_t size)
{
    assert(handle == 42 && data != NULL && size > 0);
    total_written += size;
    return write_result;
}

esp_err_t esp_ota_end(esp_ota_handle_t handle)
{
    assert(handle == 42 && total_written > 0);
    ++end_count;
    return end_result;
}

esp_err_t esp_ota_abort(esp_ota_handle_t handle)
{
    assert(handle == 42 && end_count == 0);
    ++abort_count;
    return ESP_OK;
}

esp_err_t esp_ota_set_boot_partition(const esp_partition_t *part)
{
    assert(part == &partition && end_count == 1 && end_result == ESP_OK);
    assert(now_us < deadline_us);
    ++boot_count;
    return boot_result;
}

static void expect_failure(esp_err_t expected)
{
    assert(ota_download_verified(url) == expected);
    assert(open_count == 1 && cleanup_count == 1 && destroy_count == 1);
    assert(boot_count == 0);
}

static void test_success(void)
{
    reset();
    assert(ota_download_verified(url) == ESP_OK);
    assert(total_written == 8 && begin_count == 1 && end_count == 1);
    assert(boot_count == 1 && abort_count == 0 && destroy_count == 1);
    reset();
    content_length = 0; // Chunked response; completion is checked at EOF.
    assert(ota_download_verified(url) == ESP_OK);
    assert(total_written == 8 && boot_count == 1);
}

static void test_authorization_and_redirects(void)
{
    reset();
    assert(ota_download_verified("https://elsewhere.invalid/image.bin") == ESP_ERR_INVALID_ARG);
    assert(open_count == 0 && begin_count == 0);
    const int rejected_statuses[] = {301, 302, 303, 307, 308, 206, 404, 500};
    for (size_t i = 0; i < sizeof(rejected_statuses) / sizeof(rejected_statuses[0]); ++i) {
        reset();
        status_code = rejected_statuses[i];
        expect_failure(ESP_FAIL);
        assert(begin_count == 0 && read_index == 0);
    }
}

static void test_timeouts_and_truncation(void)
{
    reset();
    content_length = -ESP_ERR_HTTP_EAGAIN;
    expect_failure(ESP_ERR_TIMEOUT);
    assert(begin_count == 0);
    reset();
    reads[0].bytes = -ESP_ERR_HTTP_EAGAIN; // No image header ever arrives.
    expect_failure(ESP_ERR_TIMEOUT);
    assert(read_index == 1 && abort_count == 1);
    reset();
    reads[1].bytes = -ESP_ERR_HTTP_EAGAIN; // Body stalls after progress.
    expect_failure(ESP_ERR_TIMEOUT);
    assert(abort_count == 1);
    reset();
    reads[0].elapsed_us = INT64_C(300000000); // Overall limit despite data.
    expect_failure(ESP_ERR_TIMEOUT);
    assert(total_written == 0 && abort_count == 1);
    reset();
    reads[0].bytes = 0;
    expect_failure(ESP_FAIL); // Empty response must not be finalized.
    assert(end_count == 0 && abort_count == 1);
    reset();
    complete = false;
    expect_failure(ESP_FAIL);
    assert(end_count == 0 && abort_count == 1);
    reset();
    content_length = 9;
    expect_failure(ESP_FAIL); // A short body cannot select the next image.
}

static void test_flash_errors_and_limits(void)
{
    reset();
    partition_present = false;
    expect_failure(ESP_ERR_NOT_FOUND);
    assert(begin_count == 0);
    reset();
    content_length = 8193;
    expect_failure(ESP_ERR_INVALID_SIZE);
    assert(begin_count == 0);
    reset();
    content_length = 0;
    partition.size = 7;
    expect_failure(ESP_ERR_INVALID_SIZE);
    assert(total_written == 0 && abort_count == 1);
    reset();
    begin_result = ESP_FAIL;
    expect_failure(ESP_FAIL);
    assert(abort_count == 0);
    reset();
    write_result = ESP_FAIL;
    expect_failure(ESP_FAIL);
    assert(abort_count == 1 && end_count == 0);
    reset();
    end_result = ESP_FAIL; // Image/signature verification failure consumes handle.
    expect_failure(ESP_FAIL);
    assert(end_count == 1 && abort_count == 0);
    reset();
    boot_result = ESP_FAIL;
    assert(ota_download_verified(url) == ESP_FAIL);
    assert(end_count == 1 && abort_count == 0 && destroy_count == 1);
    reset();
    open_result = ESP_FAIL;
    expect_failure(ESP_FAIL);
    assert(begin_count == 0);
}

int main(void)
{
    test_success();
    test_authorization_and_redirects();
    test_timeouts_and_truncation();
    test_flash_errors_and_limits();
    puts("OTA download host tests passed");
    return 0;
}
