#pragma once

#include <stdint.h>

typedef uint32_t TickType_t;
typedef unsigned int UBaseType_t;
typedef int BaseType_t;
typedef struct { int locked; } portMUX_TYPE;

#define pdFALSE 0
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portMUX_INITIALIZER_UNLOCKED {0}

void scheduler_test_enter_critical(portMUX_TYPE *lock);
void scheduler_test_exit_critical(portMUX_TYPE *lock);

#define portENTER_CRITICAL(lock) scheduler_test_enter_critical(lock)
#define portEXIT_CRITICAL(lock) scheduler_test_exit_critical(lock)
