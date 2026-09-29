
/* -----------------------------------------------------------------------
* State machine
* ----------------------------------------------------------------------- */

#ifndef __FSM__
#define __FSM__

/* ---- FreeRTOS (must come before any IDF header that uses FreeRTOS types) ---- */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

/* ---- IDF / system ---- */
#include <string>
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

#include "tg_utils.hpp"
#include "secs.hpp"
#include "types.hpp"

#define NUM_STATES 4

void oled_update(const char *l1, const char *l2);  /* forward declaration */

extern SemaphoreHandle_t state_mtx;

void state_to_string(state_t s, char *buf, size_t buf_size);

void set_state(state_t new_state, std::string id);

void next_state(std::string id);


#endif