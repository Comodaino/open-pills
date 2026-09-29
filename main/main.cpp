/* ---- FreeRTOS (must come before any IDF header that uses FreeRTOS types) ---- */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

/* ---- IDF / system ---- */
#include <string>
#include <vector>
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

/* ---- k0i05/esp_ssd1306 (installed via idf.py add-dependency) ---- */
#include "ssd1306.h"
/* ---- local libraries ---------------------------------------------*/
#include "tg_utils.hpp"
#include "fsm.hpp"
#include "types.hpp"
/* -----------------------------------------------------------------------
 * CREDENTIALS
 * ----------------------------------------------------------------------- */
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#include "esp_err.h"

/*------------------------------------------------------------------------
 * Global variable declaration
 * ----------------------------------------------------------------------- */
SemaphoreHandle_t state_mtx = NULL;
SemaphoreHandle_t tg_send_mtx = NULL;
secrets_t sec;
std::list<tg_user_t> users;
char output_buffer[MAX_HTTP_OUTPUT_BUFFER];

/* -----------------------------------------------------------------------
 * Wi-Fi
 * ----------------------------------------------------------------------- */
static EventGroupHandle_t s_wifi_eg;
#define WIFI_CONNECTED_BIT BIT0

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    s_wifi_eg = xEventGroupCreate();
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        wifi_event_handler, NULL, NULL);
    wifi_config_t wc = {};
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    strncpy((char*)wc.sta.ssid,     sec.wifi_ssid, sizeof(wc.sta.ssid));
    strncpy((char*)wc.sta.password, sec.wifi_pass, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    xEventGroupWaitBits(s_wifi_eg, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
}

/* -----------------------------------------------------------------------
 * OLED display via I2C
 * ----------------------------------------------------------------------- */
#define OLED_SDA  GPIO_NUM_4
#define OLED_SCL  GPIO_NUM_15
#define OLED_RST  GPIO_NUM_16
#define OLED_ADDR 0x3C

static i2c_master_bus_handle_t s_i2c_bus;
static ssd1306_handle_t        s_oled;

static void oled_init(void)
{
    gpio_reset_pin(OLED_RST);
    gpio_set_direction(OLED_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(OLED_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(OLED_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port                     = I2C_NUM_0;
    bus_cfg.sda_io_num                   = OLED_SDA;
    bus_cfg.scl_io_num                   = OLED_SCL;
    bus_cfg.clk_source                   = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt            = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_i2c_bus));

    /* k0i05/esp_ssd1306 init — adjust if your component version differs */
    ssd1306_config_t oled_cfg = {
        .i2c_address = OLED_ADDR,
        .i2c_clock_speed = 100000,
        .panel_size = SSD1306_PANEL_128x64,
        .offset_x = 0,
        .flip_enabled = false,
        .display_enabled = true
    };
    ESP_ERROR_CHECK(ssd1306_init(s_i2c_bus, &oled_cfg, &s_oled));
    ssd1306_clear_display(s_oled, false);
}

/* Forward declaration so oled_task (below) can call this */
static void oled_show_status(const char *line1, const char *line2);

typedef struct { char line1[32]; char line2[32]; } oled_msg_t;
static QueueHandle_t oled_q;

static void oled_task(void *arg)
{
    oled_init();
    oled_msg_t m;
    while (1) {
        if (xQueueReceive(oled_q, &m, pdMS_TO_TICKS(2000)) == pdTRUE) {
            oled_show_status(m.line1, m.line2);
        }
    }
}

/* Definition — must be void, must have a body, must come after ssd1306 init */
static void oled_show_status(const char *line1, const char *line2)
{
    ssd1306_clear_display(s_oled, false);
    /* Row 0 = line1, row 2 = line2 (each row is 8px tall) */
    ssd1306_display_text(s_oled, 0, line1, false);
    ssd1306_display_text(s_oled, 2, line2, false);
}

/* Helper: push a display update from any task */
void oled_update(const char *l1, const char *l2)
{
    oled_msg_t m;
    strncpy(m.line1, l1, sizeof(m.line1) - 1);
    strncpy(m.line2, l2, sizeof(m.line2) - 1);
    m.line1[sizeof(m.line1) - 1] = '\0';
    m.line2[sizeof(m.line2) - 1] = '\0';
    xQueueSend(oled_q, &m, 0);   /* non-blocking; drop if queue full */
}

/* -----------------------------------------------------------------------
 * Time keeping
 * ----------------------------------------------------------------------- */

#include "esp_sntp.h"
#include <time.h>
#include <sys/time.h>

void initialize_sntp() {
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}

void time_init() {
    initialize_sntp();
    // Wait for time to be set before proceeding
    time_t now = 0;
    struct tm timeinfo = {};
    int retries = 0;
    char strftime_buf[64];

    while (timeinfo.tm_year < (2020 - 1900) && retries++ < 30) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        time(&now);
        localtime_r(&now, &timeinfo);
    }

    time(&now);
    // Set timezone to Rome Standard Time
    setenv("TZ", "UTC-2", 1);
    tzset();

    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI("time", "The current date/time in Rome is: %s", strftime_buf);
}

static void set_urgent_state(std::string id)
{
    set_state(STATE_URGENT, id);
    ESP_LOGI("alarm", "21:00 — becomes urgent!");
    telegram_send("URGENT, pills not taken!", id);
}

void time_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(10000));
    while (1) {
        time_t now;
        struct tm ti;
        char hms[16];
        char h[16];
        char last_h[16];
        
        time(&now);
        localtime_r(&now, &ti);
        strftime(hms, sizeof(hms), "%H:%M:%S", &ti);
        strftime(h, sizeof(h), "%H", &ti);
        
        for(tg_user_t user : users){
            
            // 1. Midnight reset: If the hour rolls over from 23 to 00
            if (strcmp(h, "00") == 0 && strcmp(last_h, "23") == 0) {
                set_state(STATE_IDLE, user.chat_id);
            } 
            // 2. Urgent reminders: Only if they haven't taken pills today
            else if (user.curr_state != STATE_TAKEN && strcmp(hms, "21:00:00") >= 0) {
                
                // Trigger if they are just entering the time window, OR the hour just changed
                if (user.curr_state == STATE_IDLE || strcmp(h, last_h) > 0) {
                    set_urgent_state(user.chat_id);
                }
            }
        }
        strcpy(last_h,h);
        vTaskDelay(pdMS_TO_TICKS(60000)); // Update every minute
    }
}


/* -----------------------------------------------------------------------
 * Entry point
 * ----------------------------------------------------------------------- */
extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());   // must come first

    ESP_ERROR_CHECK(secrets_load(&sec));
    oled_q = xQueueCreate(8, sizeof(oled_msg_t));
    xTaskCreate(oled_task, "oled", 4096, NULL, 3, NULL);

    // Init the state mutex before any tasks start using it
    state_mtx = xSemaphoreCreateMutex();
    tg_send_mtx = xSemaphoreCreateMutex();
    oled_update("Initializing...", "Please wait");
    wifi_init_sta();           /* blocks until IP obtained */

    oled_update("Connected", "Please wait");
    time_init();
    xTaskCreate(time_task, "time", 8192, NULL, 1, NULL);

    xTaskCreate(telegram_poll_task, "tg_rx", 8192, NULL, 2, NULL);
}