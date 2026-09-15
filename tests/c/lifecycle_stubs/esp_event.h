#pragma once
#include <stdint.h>
typedef const char *esp_event_base_t;
typedef void (*esp_event_handler_t)(void *, esp_event_base_t, int32_t, void *);
#define ESP_EVENT_ANY_ID (-1)

