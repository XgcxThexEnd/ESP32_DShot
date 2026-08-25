/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_check.h"
#include "dshot_esc_encoder.h"
#include "dshot_protocol.h"

static const char *TAG = "dshot_encoder";

#define DSHOT_RMT_DURATION_MAX 0x7FFFU
#define DSHOT_USEC_PER_SEC 1000000U

typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    rmt_symbol_word_t dshot_delay_symbol;
    int state;
} rmt_dshot_esc_encoder_t;

RMT_ENCODER_FUNC_ATTR
static size_t rmt_encode_dshot_esc(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                                   const void *primary_data, size_t data_size, rmt_encode_state_t *ret_state)
{
    rmt_dshot_esc_encoder_t *dshot_encoder = __containerof(encoder, rmt_dshot_esc_encoder_t, base);
    rmt_encoder_handle_t bytes_encoder = dshot_encoder->bytes_encoder;
    rmt_encoder_handle_t copy_encoder = dshot_encoder->copy_encoder;
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    rmt_encode_state_t state = RMT_ENCODING_RESET;
    size_t encoded_symbols = 0;

    // convert user data into dshot frame
    dshot_esc_throttle_t *throttle = (dshot_esc_throttle_t *)primary_data;
    uint8_t frame[2] = {0};
    // Invalid input fails safe to the all-zero stop frame. Application policy
    // already constrains values to 0 or 48..2047, but the encoder remains safe
    // if a caller violates that contract.
    (void)dshot_protocol_build_frame(throttle->throttle, throttle->telemetry_req, frame);

    switch (dshot_encoder->state) {
    case 0: // send the dshot frame
        encoded_symbols += bytes_encoder->encode(bytes_encoder, channel, frame, sizeof(frame), &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            dshot_encoder->state = 1; // switch to next state when current encoding session finished
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            goto out; // yield if there's no free space for encoding artifacts
        }
    // fall-through
    case 1:
        encoded_symbols += copy_encoder->encode(copy_encoder, channel, &dshot_encoder->dshot_delay_symbol,
                                                sizeof(rmt_symbol_word_t), &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            state |= RMT_ENCODING_COMPLETE;
            dshot_encoder->state = RMT_ENCODING_RESET; // switch to next state when current encoding session finished
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            goto out; // yield if there's no free space for encoding artifacts
        }
    }
out:
    *ret_state = state;
    return encoded_symbols;
}

static esp_err_t rmt_del_dshot_encoder(rmt_encoder_t *encoder)
{
    rmt_dshot_esc_encoder_t *dshot_encoder = __containerof(encoder, rmt_dshot_esc_encoder_t, base);
    rmt_del_encoder(dshot_encoder->bytes_encoder);
    rmt_del_encoder(dshot_encoder->copy_encoder);
    free(dshot_encoder);
    return ESP_OK;
}

RMT_ENCODER_FUNC_ATTR
static esp_err_t rmt_dshot_encoder_reset(rmt_encoder_t *encoder)
{
    rmt_dshot_esc_encoder_t *dshot_encoder = __containerof(encoder, rmt_dshot_esc_encoder_t, base);
    rmt_encoder_reset(dshot_encoder->bytes_encoder);
    rmt_encoder_reset(dshot_encoder->copy_encoder);
    dshot_encoder->state = RMT_ENCODING_RESET;
    return ESP_OK;
}

esp_err_t rmt_new_dshot_esc_encoder(const dshot_esc_encoder_config_t *config, rmt_encoder_handle_t *ret_encoder)
{
    esp_err_t ret = ESP_OK;
    rmt_dshot_esc_encoder_t *dshot_encoder = NULL;
    ESP_GOTO_ON_FALSE(config && ret_encoder, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");
    *ret_encoder = NULL;

    ESP_GOTO_ON_FALSE(config->resolution > 0 && config->baud_rate > 0 && config->post_delay_us > 0,
                      ESP_ERR_INVALID_ARG, err, TAG, "timing values must be non-zero");

    const uint64_t period_ticks = (uint64_t)config->resolution / config->baud_rate;
    const uint64_t delay_ticks = ((uint64_t)config->resolution * config->post_delay_us) / DSHOT_USEC_PER_SEC;
    ESP_GOTO_ON_FALSE(period_ticks > 0 && delay_ticks >= 2 && delay_ticks <= (2U * DSHOT_RMT_DURATION_MAX),
                      ESP_ERR_INVALID_ARG, err, TAG, "timing values are outside the RMT range");

    const uint32_t t1h_ticks = ((uint64_t)config->resolution * 7485U) /
                               ((uint64_t)config->baud_rate * 10000U);
    const uint32_t t0h_ticks = ((uint64_t)config->resolution * 37425U) /
                               ((uint64_t)config->baud_rate * 100000U);
    const uint32_t t1l_ticks = period_ticks - t1h_ticks;
    const uint32_t t0l_ticks = period_ticks - t0h_ticks;
    ESP_GOTO_ON_FALSE(t1h_ticks > 0 && t1h_ticks <= DSHOT_RMT_DURATION_MAX &&
                      t1l_ticks > 0 && t1l_ticks <= DSHOT_RMT_DURATION_MAX &&
                      t0h_ticks > 0 && t0h_ticks <= DSHOT_RMT_DURATION_MAX &&
                      t0l_ticks > 0 && t0l_ticks <= DSHOT_RMT_DURATION_MAX,
                      ESP_ERR_INVALID_ARG, err, TAG, "DShot pulse width is outside the RMT range");

    dshot_encoder = rmt_alloc_encoder_mem(sizeof(rmt_dshot_esc_encoder_t));
    ESP_GOTO_ON_FALSE(dshot_encoder, ESP_ERR_NO_MEM, err, TAG, "no memory for DShot encoder");
    dshot_encoder->base.encode = rmt_encode_dshot_esc;
    dshot_encoder->base.del = rmt_del_dshot_encoder;
    dshot_encoder->base.reset = rmt_dshot_encoder_reset;
    rmt_symbol_word_t dshot_delay_symbol = {
        .level0 = 0,
        .duration0 = (uint32_t)(delay_ticks / 2),
        .level1 = 0,
        .duration1 = (uint32_t)(delay_ticks - (delay_ticks / 2)),
    };
    dshot_encoder->dshot_delay_symbol = dshot_delay_symbol;
    // 1 and 0 is represented by a 74.850% and 37.425% duty cycle respectively
    rmt_bytes_encoder_config_t bytes_encoder_config = {
        .bit0 = {
            .level0 = 1,
            .duration0 = t0h_ticks,
            .level1 = 0,
            .duration1 = t0l_ticks,
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = t1h_ticks,
            .level1 = 0,
            .duration1 = t1l_ticks,
        },
        .flags.msb_first = 1,
    };
    ESP_GOTO_ON_ERROR(rmt_new_bytes_encoder(&bytes_encoder_config, &dshot_encoder->bytes_encoder), err, TAG, "create bytes encoder failed");
    rmt_copy_encoder_config_t copy_encoder_config = {};
    ESP_GOTO_ON_ERROR(rmt_new_copy_encoder(&copy_encoder_config, &dshot_encoder->copy_encoder), err, TAG, "create copy encoder failed");
    *ret_encoder = &dshot_encoder->base;
    return ESP_OK;
err:
    if (dshot_encoder) {
        if (dshot_encoder->bytes_encoder) {
            rmt_del_encoder(dshot_encoder->bytes_encoder);
        }
        if (dshot_encoder->copy_encoder) {
            rmt_del_encoder(dshot_encoder->copy_encoder);
        }
        free(dshot_encoder);
    }
    return ret;
}
