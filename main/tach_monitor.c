#include "tach_monitor.h"

#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

#include "sdkconfig.h"

#ifdef CONFIG_TACH_FEEDBACK_ENABLED

#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

#define TACH_MONITOR_TASK_STACK    4096
#define TACH_MONITOR_TASK_PRIORITY 6
#define TACH_COUNTER_RESET_THRESHOLD (INT_MAX / 2)
#define TACH_INVALID_TIMEOUT_US \
    ((int64_t)CONFIG_TACH_SAMPLE_MS * 3 * 1000)

typedef struct {
    pcnt_unit_handle_t unit;
    pcnt_channel_handle_t channel;
    bool unit_enabled;
    bool unit_started;
    int previous_pulse_count;
    uint32_t measured_rpm;
    bool measurement_valid;
    bool stall_alarm;
    bool stall_latched;
    uint32_t stall_count;
    uint32_t fault_generation;
    int64_t stall_since_us;
    int64_t invalid_since_us;
    int64_t sampled_at_us;
    tach_monitor_fault_cause_t fault_cause;
} tach_fan_state_t;

static const char *TAG = "tach_monitor";
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static tach_fan_state_t s_fans[APP_CONFIG_MAX_FANS];
static const app_config_t *s_app_config;
static tach_monitor_read_motor_snapshot_fn s_read_motor_snapshot;
static tach_monitor_confirmed_stall_fn s_confirmed_stall;
static void *s_callback_context;
static TaskHandle_t s_task;
static bool s_init_started;
static bool s_initialized;
static bool s_start_started;
static int64_t s_heartbeat_us;

static tach_monitor_stall_action_t configured_stall_action(void)
{
#if defined(CONFIG_TACH_STALL_STOP_ALL)
    return TACH_MONITOR_STALL_STOP_ALL;
#elif defined(CONFIG_TACH_STALL_STOP_FAN)
    return TACH_MONITOR_STALL_STOP_FAN;
#else
    return TACH_MONITOR_STALL_ALARM_ONLY;
#endif
}

static uint32_t advance_fault_generation_locked(tach_fan_state_t *fan)
{
    fan->fault_generation++;
    // Reserve zero as the never-faulted initialization value. Wrap is
    // practically unreachable, but keeping it reserved makes diagnostics and
    // stale-event checks unambiguous.
    if (fan->fault_generation == 0) fan->fault_generation = 1;
    return fan->fault_generation;
}

static esp_err_t release_pcnt_resources(void)
{
    esp_err_t first_error = ESP_OK;
    for (int i = APP_CONFIG_MAX_FANS - 1; i >= 0; --i) {
        tach_fan_state_t *fan = &s_fans[i];
        if (fan->unit_started) {
            esp_err_t err = pcnt_unit_stop(fan->unit);
            if (err == ESP_OK) fan->unit_started = false;
            if (err != ESP_OK && first_error == ESP_OK) first_error = err;
        }
        if (fan->unit_enabled) {
            esp_err_t err = pcnt_unit_disable(fan->unit);
            if (err == ESP_OK) fan->unit_enabled = false;
            if (err != ESP_OK && first_error == ESP_OK) first_error = err;
        }
        if (fan->channel) {
            esp_err_t err = pcnt_del_channel(fan->channel);
            if (err == ESP_OK) fan->channel = NULL;
            if (err != ESP_OK && first_error == ESP_OK) first_error = err;
        }
        if (fan->unit) {
            esp_err_t err = pcnt_del_unit(fan->unit);
            if (err == ESP_OK) fan->unit = NULL;
            if (err != ESP_OK && first_error == ESP_OK) first_error = err;
        }
    }
    return first_error;
}

static esp_err_t fail_initialization(esp_err_t cause)
{
    esp_err_t cleanup_error = release_pcnt_resources();
    if (cleanup_error == ESP_OK) {
        memset(s_fans, 0, sizeof(s_fans));
        s_app_config = NULL;
        s_read_motor_snapshot = NULL;
        s_confirmed_stall = NULL;
        s_callback_context = NULL;
        portENTER_CRITICAL(&s_state_lock);
        s_init_started = false;
        portEXIT_CRITICAL(&s_state_lock);
    } else {
        // Retain all undeleted handles and poison retries instead of losing
        // ownership of partially configured PCNT resources.
        ESP_LOGE(TAG, "PCNT initialization unwind failed: %s",
                 esp_err_to_name(cleanup_error));
    }
    return cause != ESP_OK ? cause : cleanup_error;
}

static void latch_configured_stall_action_locked(int local_fan)
{
#if defined(CONFIG_TACH_STALL_STOP_ALL)
    for (int i = 0; i < s_app_config->fan_count; ++i) {
        s_fans[i].stall_latched = true;
    }
#elif defined(CONFIG_TACH_STALL_STOP_FAN)
    s_fans[local_fan].stall_latched = true;
#else
    (void)local_fan;
#endif
}

static esp_err_t read_pulse_delta(tach_fan_state_t *fan,
                                  uint64_t *out_pulse_delta)
{
    if (!fan || !out_pulse_delta) return ESP_ERR_INVALID_ARG;

    // PCNT's accumulated count is sampled cumulatively. Clearing after every
    // read creates a window where a pulse can arrive between the two calls and
    // be discarded. Reset only well before the signed accumulator limit, with
    // the unit stopped so the final pre-reset count is stable.
    if (!fan->unit_started) {
        esp_err_t clear_err = pcnt_unit_clear_count(fan->unit);
        if (clear_err != ESP_OK) return clear_err;
        fan->previous_pulse_count = 0;
        esp_err_t start_err = pcnt_unit_start(fan->unit);
        if (start_err != ESP_OK) return start_err;
        fan->unit_started = true;
        return ESP_ERR_INVALID_STATE;
    }

    const bool reset_after_read =
        fan->previous_pulse_count >= TACH_COUNTER_RESET_THRESHOLD;
    if (reset_after_read) {
        esp_err_t stop_err = pcnt_unit_stop(fan->unit);
        if (stop_err != ESP_OK) return stop_err;
        fan->unit_started = false;
    }

    int cumulative_pulses = 0;
    esp_err_t count_err = pcnt_unit_get_count(fan->unit, &cumulative_pulses);
    bool delta_valid = count_err == ESP_OK &&
                       cumulative_pulses >= fan->previous_pulse_count;
    uint64_t pulse_delta = delta_valid
                               ? (uint64_t)(cumulative_pulses -
                                            fan->previous_pulse_count)
                               : 0;

    if (!reset_after_read) {
        if (count_err != ESP_OK) return count_err;
        if (!delta_valid) return ESP_ERR_INVALID_STATE;
        fan->previous_pulse_count = cumulative_pulses;
        *out_pulse_delta = pulse_delta;
        return ESP_OK;
    }

    // Always try to bring the stopped unit back online, even if reading or
    // clearing failed. A failed restart is retried at the next sample.
    esp_err_t clear_err = pcnt_unit_clear_count(fan->unit);
    if (clear_err == ESP_OK) {
        fan->previous_pulse_count = 0;
    } else if (count_err == ESP_OK) {
        fan->previous_pulse_count = cumulative_pulses;
    }
    esp_err_t start_err = pcnt_unit_start(fan->unit);
    if (start_err == ESP_OK) fan->unit_started = true;

    if (count_err != ESP_OK) return count_err;
    if (!delta_valid) return ESP_ERR_INVALID_STATE;
    if (clear_err != ESP_OK) return clear_err;
    if (start_err != ESP_OK) return start_err;
    *out_pulse_delta = pulse_delta;
    return ESP_OK;
}

static void sample_fan(int local_fan, int64_t now, int64_t elapsed_us)
{
    tach_fan_state_t *fan = &s_fans[local_fan];
    uint64_t pulses = 0;
    bool measurement_valid = read_pulse_delta(fan, &pulses) == ESP_OK;
    uint32_t rpm = 0;
    if (measurement_valid) {
        uint64_t numerator = pulses * 60000000ULL;
        uint64_t denominator =
            (uint64_t)elapsed_us * CONFIG_TACH_PULSES_PER_REV;
        uint64_t calculated = denominator ? numerator / denominator : 0;
        rpm = calculated > UINT32_MAX ? UINT32_MAX : (uint32_t)calculated;
    }

    tach_monitor_motor_snapshot_t motor_snapshot = {0};
    bool motor_snapshot_valid =
        s_read_motor_snapshot(local_fan, &motor_snapshot, s_callback_context);
    bool startup_grace_elapsed =
        motor_snapshot_valid && motor_snapshot.applied_pct > 0 &&
        motor_snapshot.nonzero_applied_since_us > 0 &&
        now - motor_snapshot.nonzero_applied_since_us >=
            (int64_t)CONFIG_TACH_STARTUP_GRACE_MS * 1000;
    bool monitoring = measurement_valid && startup_grace_elapsed;
    bool relevant_invalid_sample =
        !motor_snapshot_valid ||
        (startup_grace_elapsed && !measurement_valid);

    bool newly_faulted = false;
    uint32_t stall_count = 0;
    uint32_t fault_generation = 0;
    tach_monitor_fault_cause_t fault_cause = TACH_MONITOR_FAULT_NONE;
    portENTER_CRITICAL(&s_state_lock);
    fan->measurement_valid = measurement_valid && motor_snapshot_valid;
    fan->measured_rpm = rpm;
    fan->sampled_at_us = now;
    if (relevant_invalid_sample) {
        if (fan->invalid_since_us == 0) fan->invalid_since_us = now;
        if (!fan->stall_alarm &&
            now - fan->invalid_since_us >= TACH_INVALID_TIMEOUT_US) {
            fan->stall_alarm = true;
            fan->fault_cause = TACH_MONITOR_FAULT_SENSOR_INVALID;
            if (fan->stall_count < UINT32_MAX) fan->stall_count++;
            stall_count = fan->stall_count;
            fault_generation = advance_fault_generation_locked(fan);
            fault_cause = fan->fault_cause;
            latch_configured_stall_action_locked(local_fan);
            newly_faulted = true;
        }
    } else if (monitoring && rpm < CONFIG_TACH_STALL_RPM) {
        fan->invalid_since_us = 0;
        if (fan->stall_since_us == 0) fan->stall_since_us = now;
        if (!fan->stall_alarm &&
            now - fan->stall_since_us >=
                (int64_t)CONFIG_TACH_STALL_DEBOUNCE_MS * 1000) {
            fan->stall_alarm = true;
            fan->fault_cause = TACH_MONITOR_FAULT_STALL;
            if (fan->stall_count < UINT32_MAX) fan->stall_count++;
            stall_count = fan->stall_count;
            fault_generation = advance_fault_generation_locked(fan);
            fault_cause = fan->fault_cause;
            latch_configured_stall_action_locked(local_fan);
            newly_faulted = true;
        }
    } else {
        fan->invalid_since_us = 0;
        fan->stall_since_us = 0;
        if (!fan->stall_latched) {
            fan->stall_alarm = false;
            fan->fault_cause = TACH_MONITOR_FAULT_NONE;
        }
    }
    portEXIT_CRITICAL(&s_state_lock);

    if (!newly_faulted) return;

    tach_monitor_stall_event_t event = {
        .local_fan = local_fan,
        .fan_number = s_app_config->fan_index_start + local_fan,
        .measured_rpm = rpm,
        .stall_count = stall_count,
        .fault_generation = fault_generation,
        .action = configured_stall_action(),
        .cause = fault_cause,
    };
    if (event.cause == TACH_MONITOR_FAULT_SENSOR_INVALID) {
        ESP_LOGE(TAG, "fan%d tach feedback invalid while output may be active",
                 event.fan_number);
    } else {
        ESP_LOGE(TAG, "fan%d tach stall: measured=%" PRIu32 " rpm",
                 event.fan_number, event.measured_rpm);
    }
    // Never invoke another module while holding the tach state lock.
    s_confirmed_stall(&event, s_callback_context);
}

static void tach_monitor_task(void *arg)
{
    (void)arg;
    int64_t previous_sample_us = esp_timer_get_time();
    const TickType_t configured_delay = pdMS_TO_TICKS(CONFIG_TACH_SAMPLE_MS);
    const TickType_t sample_delay = configured_delay > 0 ? configured_delay : 1;

    while (true) {
        vTaskDelay(sample_delay);
        int64_t now = esp_timer_get_time();
        int64_t elapsed_us = now - previous_sample_us;
        previous_sample_us = now;
        if (elapsed_us <= 0) continue;

        for (int i = 0; i < s_app_config->fan_count; ++i) {
            sample_fan(i, now, elapsed_us);
        }
        portENTER_CRITICAL(&s_state_lock);
        s_heartbeat_us = esp_timer_get_time();
        portEXIT_CRITICAL(&s_state_lock);
    }
}

esp_err_t tach_monitor_init(const tach_monitor_config_t *config)
{
    if (!config || !config->app_config || !config->read_motor_snapshot ||
        !config->confirmed_stall) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->app_config->fan_count < 1 ||
        config->app_config->fan_count > APP_CONFIG_MAX_FANS) {
        return ESP_ERR_INVALID_ARG;
    }

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

    s_app_config = config->app_config;
    s_read_motor_snapshot = config->read_motor_snapshot;
    s_confirmed_stall = config->confirmed_stall;
    s_callback_context = config->callback_context;

    for (int i = 0; i < s_app_config->fan_count; ++i) {
        gpio_pull_mode_t pull_mode =
#ifdef CONFIG_TACH_INTERNAL_PULLUP
            GPIO_PULLUP_ONLY;
#else
            GPIO_FLOATING;
#endif
        esp_err_t err = gpio_set_pull_mode(s_app_config->tach_gpios[i], pull_mode);
        if (err != ESP_OK) {
            return fail_initialization(err);
        }

        pcnt_unit_config_t unit_config = {
            .low_limit = -1,
            .high_limit = INT16_MAX,
            // Accumulate through hardware watchpoints so a long sample window
            // or high-PPR motor cannot wrap into a false stall indication.
            .flags.accum_count = true,
        };
        tach_fan_state_t *fan = &s_fans[i];
        err = pcnt_new_unit(&unit_config, &fan->unit);
        if (err != ESP_OK) {
            return fail_initialization(err);
        }
        if (CONFIG_TACH_GLITCH_FILTER_NS > 0) {
            pcnt_glitch_filter_config_t filter = {
                .max_glitch_ns = CONFIG_TACH_GLITCH_FILTER_NS,
            };
            err = pcnt_unit_set_glitch_filter(fan->unit, &filter);
            if (err != ESP_OK) {
                return fail_initialization(err);
            }
        }

        pcnt_chan_config_t channel_config = {
            .edge_gpio_num = s_app_config->tach_gpios[i],
            .level_gpio_num = -1,
        };
        err = pcnt_new_channel(fan->unit, &channel_config, &fan->channel);
        if (err == ESP_OK) {
            err = pcnt_channel_set_edge_action(
                fan->channel, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                PCNT_CHANNEL_EDGE_ACTION_HOLD);
        }
        if (err == ESP_OK) err = pcnt_unit_add_watch_point(fan->unit, INT16_MAX);
        if (err == ESP_OK) err = pcnt_unit_add_watch_point(fan->unit, -1);
        if (err == ESP_OK) {
            err = pcnt_unit_enable(fan->unit);
            if (err == ESP_OK) fan->unit_enabled = true;
        }
        if (err == ESP_OK) err = pcnt_unit_clear_count(fan->unit);
        if (err == ESP_OK) {
            err = pcnt_unit_start(fan->unit);
            if (err == ESP_OK) fan->unit_started = true;
        }
        if (err != ESP_OK) {
            return fail_initialization(err);
        }
    }

    portENTER_CRITICAL(&s_state_lock);
    s_initialized = true;
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t tach_monitor_start(void)
{
    portENTER_CRITICAL(&s_state_lock);
    if (!s_initialized) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_task) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_OK;
    }
    if (s_start_started) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_start_started = true;
    portEXIT_CRITICAL(&s_state_lock);

    TaskHandle_t task = NULL;
    if (xTaskCreate(tach_monitor_task, "tach", TACH_MONITOR_TASK_STACK, NULL,
                    TACH_MONITOR_TASK_PRIORITY, &task) != pdPASS) {
        portENTER_CRITICAL(&s_state_lock);
        s_start_started = false;
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_state_lock);
    s_task = task;
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t tach_monitor_clear_alarm(int local_fan)
{
    portENTER_CRITICAL(&s_state_lock);
    if (!s_initialized || local_fan < 0 ||
        local_fan >= s_app_config->fan_count) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_ARG;
    }
    s_fans[local_fan].stall_alarm = false;
    s_fans[local_fan].stall_latched = false;
    s_fans[local_fan].stall_since_us = 0;
    s_fans[local_fan].invalid_since_us = 0;
    s_fans[local_fan].fault_cause = TACH_MONITOR_FAULT_NONE;
    (void)advance_fault_generation_locked(&s_fans[local_fan]);
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t tach_monitor_get_snapshot(int local_fan,
                                    tach_monitor_snapshot_t *out_snapshot)
{
    if (!out_snapshot) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_state_lock);
    if (!s_initialized || local_fan < 0 ||
        local_fan >= s_app_config->fan_count) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_ARG;
    }
    const tach_fan_state_t *fan = &s_fans[local_fan];
    *out_snapshot = (tach_monitor_snapshot_t) {
        .measurement_valid = fan->measurement_valid,
        .measured_rpm = fan->measured_rpm,
        .stall_alarm = fan->stall_alarm,
        .stall_latched = fan->stall_latched,
        .stall_count = fan->stall_count,
        .fault_generation = fan->fault_generation,
        .stall_since_us = fan->stall_since_us,
        .invalid_since_us = fan->invalid_since_us,
        .sampled_at_us = fan->sampled_at_us,
        .fault_cause = fan->fault_cause,
    };
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

int64_t tach_monitor_get_heartbeat_us(void)
{
    portENTER_CRITICAL(&s_state_lock);
    int64_t heartbeat = s_heartbeat_us;
    portEXIT_CRITICAL(&s_state_lock);
    return heartbeat;
}

UBaseType_t tach_monitor_get_stack_high_water_mark(void)
{
    portENTER_CRITICAL(&s_state_lock);
    TaskHandle_t task = s_task;
    portEXIT_CRITICAL(&s_state_lock);
    return task ? uxTaskGetStackHighWaterMark(task) : 0;
}

#else

esp_err_t tach_monitor_init(const tach_monitor_config_t *config)
{
    (void)config;
    return ESP_OK;
}

esp_err_t tach_monitor_start(void)
{
    return ESP_OK;
}

esp_err_t tach_monitor_clear_alarm(int local_fan)
{
    (void)local_fan;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t tach_monitor_get_snapshot(int local_fan,
                                    tach_monitor_snapshot_t *out_snapshot)
{
    (void)local_fan;
    if (!out_snapshot) return ESP_ERR_INVALID_ARG;
    *out_snapshot = (tach_monitor_snapshot_t) {0};
    return ESP_ERR_NOT_SUPPORTED;
}

int64_t tach_monitor_get_heartbeat_us(void)
{
    return 0;
}

UBaseType_t tach_monitor_get_stack_high_water_mark(void)
{
    return 0;
}

#endif
