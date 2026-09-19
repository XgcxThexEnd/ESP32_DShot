#pragma once

#include "app_config.h"
#include "mqtt_manager.h"

/** Install the immutable command configuration before starting MQTT. */
esp_err_t command_executor_init(const app_config_t *config);

/** MQTT dispatch callback; owns session/stop fencing around command mutations. */
void command_executor_on_message(void *context, const char *topic,
                                  const char *data, size_t length, bool retained,
                                  const mqtt_manager_command_session_t *session);
