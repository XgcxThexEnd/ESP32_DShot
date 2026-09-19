#pragma once
#include <stdbool.h>
#include "esp_err.h"
typedef void *pcnt_unit_handle_t;
typedef void *pcnt_channel_handle_t;
typedef struct {
    int low_limit;
    int high_limit;
    struct { bool accum_count; } flags;
} pcnt_unit_config_t;
typedef struct { unsigned max_glitch_ns; } pcnt_glitch_filter_config_t;
typedef struct { int edge_gpio_num; int level_gpio_num; } pcnt_chan_config_t;
enum { PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD };
esp_err_t pcnt_new_unit(const pcnt_unit_config_t *config, pcnt_unit_handle_t *unit);
esp_err_t pcnt_new_channel(pcnt_unit_handle_t unit, const pcnt_chan_config_t *config,
                           pcnt_channel_handle_t *channel);
esp_err_t pcnt_unit_set_glitch_filter(pcnt_unit_handle_t unit,
                                     const pcnt_glitch_filter_config_t *filter);
esp_err_t pcnt_channel_set_edge_action(pcnt_channel_handle_t channel, int pos, int neg);
esp_err_t pcnt_unit_add_watch_point(pcnt_unit_handle_t unit, int point);
esp_err_t pcnt_unit_enable(pcnt_unit_handle_t unit);
esp_err_t pcnt_unit_disable(pcnt_unit_handle_t unit);
esp_err_t pcnt_unit_start(pcnt_unit_handle_t unit);
esp_err_t pcnt_unit_stop(pcnt_unit_handle_t unit);
esp_err_t pcnt_unit_clear_count(pcnt_unit_handle_t unit);
esp_err_t pcnt_unit_get_count(pcnt_unit_handle_t unit, int *count);
esp_err_t pcnt_del_channel(pcnt_channel_handle_t channel);
esp_err_t pcnt_del_unit(pcnt_unit_handle_t unit);

