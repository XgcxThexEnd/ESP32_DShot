#include "wifi_manager.h"

#include <inttypes.h>
#include <string.h>
#include <time.h>

#include "sdkconfig.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#define WIFI_MANAGER_CONNECTED_BIT BIT0
#define WIFI_MANAGER_GOT_IP_BIT    BIT1

static const char *TAG = "wifi_manager";

static EventGroupHandle_t s_event_group;
static SemaphoreHandle_t s_retry_mutex;
static esp_netif_t *s_netif;
static esp_timer_handle_t s_retry_timer;
static esp_event_handler_instance_t s_wifi_event_instance;
static esp_event_handler_instance_t s_ip_event_instance;
static bool s_event_loop_owned;
static bool s_wifi_event_registered;
static bool s_ip_event_registered;
static bool s_wifi_driver_initialized;
static bool s_wifi_started;

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_init_started;
static bool s_initialized;
static bool s_sntp_initialized;
static bool s_connected_to_ap;
static bool s_has_ip;
static bool s_degraded;
static uint32_t s_retry_count;
static uint32_t s_disconnect_count;
static int64_t s_last_ip_us;

static void wifi_manager_event_handler(void *arg, esp_event_base_t event_base,
                                       int32_t event_id, void *event_data);
static void schedule_reconnect(void);

static void set_degraded(bool degraded)
{
    portENTER_CRITICAL(&s_state_lock);
    s_degraded = degraded;
    portEXIT_CRITICAL(&s_state_lock);
}

static esp_err_t fail_initialization(esp_err_t error)
{
    if (s_retry_timer) {
        (void)esp_timer_stop(s_retry_timer);
        (void)esp_timer_delete(s_retry_timer);
        s_retry_timer = NULL;
    }
    if (s_sntp_initialized) {
        esp_netif_sntp_deinit();
        s_sntp_initialized = false;
    }
    if (s_wifi_started) {
        (void)esp_wifi_stop();
        s_wifi_started = false;
    }
    if (s_wifi_event_registered) {
        (void)esp_event_handler_instance_unregister(
            WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_event_instance);
        s_wifi_event_registered = false;
    }
    if (s_ip_event_registered) {
        (void)esp_event_handler_instance_unregister(
            IP_EVENT, ESP_EVENT_ANY_ID, s_ip_event_instance);
        s_ip_event_registered = false;
    }
    if (s_netif) {
        esp_netif_destroy_default_wifi(s_netif);
        s_netif = NULL;
    }
    if (s_wifi_driver_initialized) {
        (void)esp_wifi_deinit();
        s_wifi_driver_initialized = false;
    }
    if (s_event_group) {
        vEventGroupDelete(s_event_group);
        s_event_group = NULL;
    }
    if (s_retry_mutex) {
        vSemaphoreDelete(s_retry_mutex);
        s_retry_mutex = NULL;
    }
    if (s_event_loop_owned) {
        (void)esp_event_loop_delete_default();
        s_event_loop_owned = false;
    }

    portENTER_CRITICAL(&s_state_lock);
    s_init_started = false;
    s_initialized = false;
    s_connected_to_ap = false;
    s_has_ip = false;
    s_degraded = true;
    portEXIT_CRITICAL(&s_state_lock);
    return error;
}

static void retry_timer_callback(void *arg)
{
    (void)arg;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi reconnect request failed: %s", esp_err_to_name(err));
        schedule_reconnect();
    }
}

static void schedule_reconnect(void)
{
    if (!s_retry_mutex || !s_retry_timer) return;
    if (xSemaphoreTake(s_retry_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to serialize Wi-Fi reconnect scheduling");
        return;
    }

    uint32_t retry_count;
    portENTER_CRITICAL(&s_state_lock);
    retry_count = s_retry_count;
    if (s_retry_count < UINT32_MAX) s_retry_count++;
    portEXIT_CRITICAL(&s_state_lock);

    uint32_t base_ms = CONFIG_WIFI_RETRY_BASE_MS;
    uint32_t max_ms = CONFIG_WIFI_RETRY_MAX_MS;
    if (max_ms < base_ms) max_ms = base_ms;
    uint32_t shift = retry_count > 16 ? 16 : retry_count;
    uint64_t expanded = (uint64_t)base_ms << shift;
    uint32_t delay_ms = expanded > max_ms ? max_ms : (uint32_t)expanded;
    uint32_t jitter_window = delay_ms / 4U;
    if (jitter_window) delay_ms += esp_random() % (jitter_window + 1U);
    if (delay_ms > max_ms) delay_ms = max_ms;

    (void)esp_timer_stop(s_retry_timer);
    esp_err_t err = esp_timer_start_once(s_retry_timer,
                                         (uint64_t)delay_ms * 1000ULL);
    xSemaphoreGive(s_retry_mutex);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to arm Wi-Fi reconnect timer: %s",
                 esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "Wi-Fi degraded; reconnect in %" PRIu32 " ms", delay_ms);
    }
}

static void stop_retry_timer_and_reset_backoff(void)
{
    if (!s_retry_mutex || !s_retry_timer) return;
    if (xSemaphoreTake(s_retry_mutex, portMAX_DELAY) != pdTRUE) return;
    (void)esp_timer_stop(s_retry_timer);
    portENTER_CRITICAL(&s_state_lock);
    s_retry_count = 0;
    portEXIT_CRITICAL(&s_state_lock);
    xSemaphoreGive(s_retry_mutex);
}

static void wifi_manager_event_handler(void *arg, esp_event_base_t event_base,
                                       int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "Wi-Fi start, connecting...");
            portENTER_CRITICAL(&s_state_lock);
            s_retry_count = 0;
            portEXIT_CRITICAL(&s_state_lock);
            if (esp_wifi_connect() != ESP_OK) schedule_reconnect();
            break;
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "Wi-Fi connected to AP");
            portENTER_CRITICAL(&s_state_lock);
            s_connected_to_ap = true;
            portEXIT_CRITICAL(&s_state_lock);
            if (s_event_group) {
                xEventGroupSetBits(s_event_group, WIFI_MANAGER_CONNECTED_BIT);
            }
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *disconnected =
                (const wifi_event_sta_disconnected_t *)event_data;
            const char *reason = "";
            if (disconnected) {
                switch (disconnected->reason) {
                case 1: reason = "UNSPECIFIED"; break;
                case 2: reason = "AUTH_EXPIRE"; break;
                case 3: reason = "AUTH_LEAVE"; break;
                case 4: reason = "ASSOC_EXPIRE"; break;
                case 5: reason = "ASSOC_TOOMANY"; break;
                case 6: reason = "NOT_AUTHED"; break;
                case 7: reason = "NOT_ASSOCED"; break;
                case 8: reason = "ASSOC_LEAVE"; break;
                case 9: reason = "ASSOC_NOT_AUTHED"; break;
                case 201: reason = "NO_AP_FOUND"; break;
                case 202: reason = "AUTH_FAIL"; break;
                case 203: reason = "ASSOC_FAIL"; break;
                case 204: reason = "HANDSHAKE_TIMEOUT"; break;
                case 205: reason = "CONNECTION_FAIL"; break;
                default: reason = "OTHER"; break;
                }
            }
            ESP_LOGW(TAG, "Wi-Fi disconnected, reason=%d (%s), retrying...",
                     disconnected ? disconnected->reason : -1, reason);
            portENTER_CRITICAL(&s_state_lock);
            s_connected_to_ap = false;
            s_has_ip = false;
            s_degraded = true;
            if (s_disconnect_count < UINT32_MAX) s_disconnect_count++;
            portEXIT_CRITICAL(&s_state_lock);
            if (s_event_group) {
                xEventGroupClearBits(s_event_group,
                                     WIFI_MANAGER_CONNECTED_BIT |
                                         WIFI_MANAGER_GOT_IP_BIT);
            }
            schedule_reconnect();
            break;
        }
        default:
            break;
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *got_ip = (const ip_event_got_ip_t *)event_data;
        if (got_ip) {
            ESP_LOGI(TAG, "Got IP: " IPSTR ", GW: " IPSTR ", Netmask: " IPSTR,
                     IP2STR(&got_ip->ip_info.ip), IP2STR(&got_ip->ip_info.gw),
                     IP2STR(&got_ip->ip_info.netmask));
        }
        stop_retry_timer_and_reset_backoff();
        portENTER_CRITICAL(&s_state_lock);
        s_connected_to_ap = true;
        s_has_ip = true;
        s_degraded = false;
        s_last_ip_us = esp_timer_get_time();
        portEXIT_CRITICAL(&s_state_lock);
        if (s_event_group) {
            xEventGroupSetBits(s_event_group,
                               WIFI_MANAGER_CONNECTED_BIT |
                                   WIFI_MANAGER_GOT_IP_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        portENTER_CRITICAL(&s_state_lock);
        s_has_ip = false;
        s_degraded = true;
        portEXIT_CRITICAL(&s_state_lock);
        if (s_event_group) {
            xEventGroupClearBits(s_event_group, WIFI_MANAGER_GOT_IP_BIT);
        }
    }
}

esp_err_t wifi_manager_init(void)
{
    portENTER_CRITICAL(&s_state_lock);
    if (s_initialized) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_OK;
    }
    if (s_init_started) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_init_started = true;
    portEXIT_CRITICAL(&s_state_lock);

    const size_t ssid_len = strlen(CONFIG_WIFI_SSID);
    const size_t password_len = strlen(CONFIG_WIFI_PASS);
    if (ssid_len == 0 || ssid_len > sizeof(((wifi_config_t *)0)->sta.ssid) ||
        password_len > sizeof(((wifi_config_t *)0)->sta.password)) {
        // Exact 32-byte SSIDs and 64-byte passwords are valid fixed-width
        // driver inputs even though they do not leave room for a trailing NUL.
        ESP_LOGE(TAG, "Wi-Fi credentials are empty or exceed driver limits");
        return fail_initialization(ESP_ERR_INVALID_SIZE);
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        return fail_initialization(err);
    }
    err = esp_event_loop_create_default();
    if (err == ESP_OK) {
        s_event_loop_owned = true;
    } else if (err != ESP_ERR_INVALID_STATE) {
        return fail_initialization(err);
    }
    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) {
        return fail_initialization(ESP_ERR_NO_MEM);
    }

    s_event_group = xEventGroupCreate();
    s_retry_mutex = xSemaphoreCreateMutex();
    if (!s_event_group || !s_retry_mutex) {
        ESP_LOGE(TAG, "Failed to allocate Wi-Fi synchronization objects");
        return fail_initialization(ESP_ERR_NO_MEM);
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_manager_event_handler, NULL,
                                              &s_wifi_event_instance);
    if (err != ESP_OK) {
        return fail_initialization(err);
    }
    s_wifi_event_registered = true;
    err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_manager_event_handler, NULL,
                                              &s_ip_event_instance);
    if (err != ESP_OK) {
        return fail_initialization(err);
    }
    s_ip_event_registered = true;

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_config);
    if (err != ESP_OK) {
        return fail_initialization(err);
    }
    s_wifi_driver_initialized = true;

    wifi_config_t wifi_config = {0};
    memcpy(wifi_config.sta.ssid, CONFIG_WIFI_SSID, ssid_len);
    memcpy(wifi_config.sta.password, CONFIG_WIFI_PASS, password_len);
#ifdef CONFIG_WIFI_REQUIRE_SECURE_AUTH
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
#else
    wifi_config.sta.threshold.authmode =
        CONFIG_WIFI_PASS[0] ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
#endif
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    wifi_config.sta.pmf_cfg.capable = true;
#ifdef CONFIG_WIFI_PMF_REQUIRED
    wifi_config.sta.pmf_cfg.required = true;
#else
    wifi_config.sta.pmf_cfg.required = false;
#endif
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        return fail_initialization(err);
    }

    // Initialize every fatal auxiliary dependency before starting the radio.
    // Once esp_wifi_start succeeds this function has no failure path that can
    // leave a live, externally visible partial manager behind.
    esp_sntp_config_t sntp_config =
        ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_TLS_SNTP_SERVER);
    err = esp_netif_sntp_init(&sntp_config);
    if (err != ESP_OK) return fail_initialization(err);
    s_sntp_initialized = true;

    esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps_err != ESP_OK) {
        // Power-save policy affects responsiveness, not correctness. Keep the
        // manager usable with the driver's default policy on unsupported chips.
        ESP_LOGW(TAG, "Wi-Fi power-save override failed: %s",
                 esp_err_to_name(ps_err));
    }

    const esp_timer_create_args_t retry_timer_args = {
        .callback = retry_timer_callback,
        .name = "wifi_retry",
    };
    err = esp_timer_create(&retry_timer_args, &s_retry_timer);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) {
        return fail_initialization(err);
    }
    s_wifi_started = true;
#ifdef CONFIG_WIFI_SET_MAX_TX_POWER
    esp_err_t tx_power_err = esp_wifi_set_max_tx_power(CONFIG_WIFI_MAX_TX_POWER_QDBM);
    if (tx_power_err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi TX-power override failed: %s",
                 esp_err_to_name(tx_power_err));
    }
#endif

    portENTER_CRITICAL(&s_state_lock);
    s_initialized = true;
    portEXIT_CRITICAL(&s_state_lock);
    ESP_LOGI(TAG, "Wi-Fi manager initialized, waiting for IP...");
    return ESP_OK;
}

bool wifi_manager_wait_for_ip(TickType_t timeout_ticks)
{
    EventGroupHandle_t event_group;
    portENTER_CRITICAL(&s_state_lock);
    event_group = s_event_group;
    portEXIT_CRITICAL(&s_state_lock);
    if (!event_group) return false;

    EventBits_t bits = xEventGroupWaitBits(event_group,
                                          WIFI_MANAGER_GOT_IP_BIT,
                                          pdFALSE, pdTRUE, timeout_ticks);
    bool has_ip = (bits & WIFI_MANAGER_GOT_IP_BIT) != 0;
    if (!has_ip) {
        // The IP event records state before setting the event bit. Recheck the
        // protected state at the timeout boundary so a concurrent GOT_IP does
        // not incorrectly leave the manager degraded.
        portENTER_CRITICAL(&s_state_lock);
        has_ip = s_has_ip;
        if (!has_ip) s_degraded = true;
        portEXIT_CRITICAL(&s_state_lock);
    }
    return has_ip;
}

bool wifi_manager_has_ip(void)
{
    bool has_ip;
    portENTER_CRITICAL(&s_state_lock);
    has_ip = s_has_ip;
    portEXIT_CRITICAL(&s_state_lock);
    return has_ip;
}

bool wifi_manager_system_time_valid_for_tls(void)
{
    time_t now = time(NULL);
    return now >= (time_t)1704067200; // 2024-01-01T00:00:00Z
}

esp_err_t wifi_manager_wait_for_time(TickType_t timeout_ticks)
{
    bool sntp_initialized;
    portENTER_CRITICAL(&s_state_lock);
    sntp_initialized = s_sntp_initialized;
    portEXIT_CRITICAL(&s_state_lock);
    if (!sntp_initialized) return ESP_ERR_INVALID_STATE;
    if (wifi_manager_system_time_valid_for_tls()) return ESP_OK;

    esp_err_t err = esp_netif_sntp_sync_wait(timeout_ticks);
    if (err == ESP_OK && !wifi_manager_system_time_valid_for_tls()) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err != ESP_OK) {
        set_degraded(true);
    } else if (wifi_manager_has_ip()) {
        set_degraded(false);
    }
    return err;
}

void wifi_manager_get_snapshot(wifi_manager_snapshot_t *out_snapshot)
{
    if (!out_snapshot) return;

    wifi_manager_snapshot_t snapshot;
    portENTER_CRITICAL(&s_state_lock);
    snapshot.initialized = s_initialized;
    snapshot.connected_to_ap = s_connected_to_ap;
    snapshot.has_ip = s_has_ip;
    snapshot.degraded = s_degraded;
    snapshot.retry_count = s_retry_count;
    snapshot.disconnect_count = s_disconnect_count;
    snapshot.last_ip_us = s_last_ip_us;
    portEXIT_CRITICAL(&s_state_lock);
    *out_snapshot = snapshot;
}
