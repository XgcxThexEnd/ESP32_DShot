#pragma once

#include "FreeRTOS.h"

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
typedef enum { eSetBits } eNotifyAction;

TickType_t xTaskGetTickCount(void);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
BaseType_t xTaskNotify(TaskHandle_t task, uint32_t value, eNotifyAction action);
BaseType_t xTaskNotifyWait(uint32_t clear_on_entry, uint32_t clear_on_exit,
                           uint32_t *value, TickType_t ticks);
BaseType_t xTaskCreate(TaskFunction_t function, const char *name,
                        uint32_t stack_depth, void *argument,
                        UBaseType_t priority, TaskHandle_t *task);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t function, const char *name,
                                    uint32_t stack_depth, void *argument,
                                    UBaseType_t priority, TaskHandle_t *task,
                                    BaseType_t core);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(TaskHandle_t task);
