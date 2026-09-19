#pragma once

#include "safety_supervisor.h"
#include "tach_monitor.h"

#define TACH_ACTION_HEARTBEAT_MS 250

/** Initialize serialization before tach_monitor_init, then start before sampling. */
esp_err_t tach_actions_init(void);
esp_err_t tach_actions_start(void);

#ifdef CONFIG_TACH_FEEDBACK_ENABLED
bool tach_actions_read_motor_snapshot(int local_fan,
                                      tach_monitor_motor_snapshot_t *out,
                                      void *context);
bool tach_actions_read_health(void *context,
                              safety_supervisor_tach_action_health_t *out);
void tach_actions_confirmed_stall(const tach_monitor_stall_event_t *event,
                                   void *context);
#endif

/** Acquire before the MQTT session fence; call clear_locked only while held. */
esp_err_t tach_actions_clear_begin(void);
esp_err_t tach_actions_clear_locked(int local_fan);
void tach_actions_clear_end(void);
