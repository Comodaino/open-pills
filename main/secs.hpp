#ifndef __SECS__
#define __SECS__

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_err.h"

typedef struct {
    char wifi_ssid[64];
    char wifi_pass[64];
    char tg_token[64];
} secrets_t;

extern secrets_t sec;

esp_err_t secrets_load(secrets_t *out);

#endif