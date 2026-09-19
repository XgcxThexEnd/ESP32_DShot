#pragma once

void scheduler_test_log(const char *tag, const char *format, ...);

#define ESP_LOGE(...) scheduler_test_log(__VA_ARGS__)
#define ESP_LOGW(...) scheduler_test_log(__VA_ARGS__)
#define ESP_LOGI(...) scheduler_test_log(__VA_ARGS__)
