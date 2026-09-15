#pragma once
#include "esp_err.h"
typedef enum { GPIO_PULLUP_ONLY, GPIO_FLOATING } gpio_pull_mode_t;
esp_err_t gpio_set_pull_mode(int gpio, gpio_pull_mode_t mode);

