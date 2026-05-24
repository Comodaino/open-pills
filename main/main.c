/* ---- FreeRTOS (must come before any IDF header that uses FreeRTOS types) ---- */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

/* ---- IDF / system ---- */
#include <string.h>
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

/* -----------------------------------------------------------------------
 * CREDENTIALS
 * ----------------------------------------------------------------------- */
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#include "esp_err.h"

typedef struct {
    char wifi_ssid[64];
    char wifi_pass[64];
    char tg_token[64];
    char tg_chat[32];
} secrets_t;

static secrets_t sec;

esp_err_t secrets_load(secrets_t *out)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("secrets", NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGE("secrets", "NVS open failed — did you run the provisioner?");
        return err;
    }

    size_t len;

    len = sizeof(out->wifi_ssid);
    ESP_ERROR_CHECK(nvs_get_str(h, "wifi_ssid", out->wifi_ssid, &len));

    len = sizeof(out->wifi_pass);
    ESP_ERROR_CHECK(nvs_get_str(h, "wifi_pass", out->wifi_pass, &len));

    len = sizeof(out->tg_token);
    ESP_ERROR_CHECK(nvs_get_str(h, "tg_token",  out->tg_token,  &len));

    len = sizeof(out->tg_chat);
    ESP_ERROR_CHECK(nvs_get_str(h, "tg_chat",   out->tg_chat,   &len));

    nvs_close(h);
    ESP_LOGI("secrets", "Secrets loaded OK");
    return ESP_OK;
}

/* -----------------------------------------------------------------------
* State machine
* ----------------------------------------------------------------------- */
#define NUM_STATES 4

void oled_update(const char *l1, const char *l2);  /* forward declaration */

typedef enum state {
    STATE_INITIALIZING = 0,
    STATE_IDLE,
    STATE_TAKEN,
    STATE_URGENT
} state_t;

static state_t curr_state = STATE_INITIALIZING;

static SemaphoreHandle_t state_mtx;

static void state_to_string(state_t s, char *buf, size_t buf_size) {
    switch (s) {
        case STATE_INITIALIZING:
            strncpy(buf, "INITIALIZING", buf_size);
            break;
        case STATE_IDLE:
            strncpy(buf, "IDLE", buf_size);
            break;
        case STATE_TAKEN:
            strncpy(buf, "TAKEN", buf_size);
            break;
        case STATE_URGENT:
            strncpy(buf, "URGENT", buf_size);
            break;
        default:
            strncpy(buf, "UNKNOWN", buf_size);
    }
}

static void set_state(state_t new_state) {
    xSemaphoreTake(state_mtx, portMAX_DELAY);
    curr_state = new_state;
    char state_str[32];
    state_to_string(new_state, state_str, sizeof(state_str));
    xSemaphoreGive(state_mtx);
}

static void next_state() {
    xSemaphoreTake(state_mtx, portMAX_DELAY);
    curr_state++;
    curr_state = curr_state % NUM_STATES;
    xSemaphoreGive(state_mtx);
}

static state_t get_state() {
    xSemaphoreTake(state_mtx, portMAX_DELAY);
    state_t s = curr_state;
    xSemaphoreGive(state_mtx);
    return s;
}

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
    wifi_config_t wc = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
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
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port               = I2C_NUM_0,
        .sda_io_num             = OLED_SDA,
        .scl_io_num             = OLED_SCL,
        .clk_source             = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt      = 7,
        .flags.enable_internal_pullup = true,
    };
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
 * Telegram
 * ----------------------------------------------------------------------- */
#define MAX_HTTP_OUTPUT_BUFFER 2048
static int64_t last_update_id = 0;
static int64_t last_message_id = 0;

static char output_buffer[MAX_HTTP_OUTPUT_BUFFER];

struct tg_update {
    int64_t update_id;
    int64_t message_id;
    char text[256];
};

void parse_telegram_response(const char *json, struct tg_update *update)
{    /* This is a very naive parser just for demonstration.  In production,
     * consider using a proper JSON library like cJSON or jsmn. */
    const char *update_id_str = "\"update_id\":";
    const char *message_id_str = "\"message_id\":";
    const char *text_str = "\"text\":\"";

    char *update_id_pos = strstr(json, update_id_str);
    char *message_id_pos = strstr(json, message_id_str);
    char *text_pos = strstr(json, text_str);

    if (update_id_pos) {
        update->update_id = atoll(update_id_pos + strlen(update_id_str));
    }
    if (message_id_pos) {
        update->message_id = atoll(message_id_pos + strlen(message_id_str));
    }
    if (text_pos) {
        text_pos += strlen(text_str);
        char *end_quote = strchr(text_pos, '"');
        if (end_quote) {
            size_t len = end_quote - text_pos;
            if (len >= sizeof(update->text)) {
                len = sizeof(update->text) - 1;
            }
            strncpy(update->text, text_pos, len);
            update->text[len] = '\0';
        }
    }
}

void telegram_send(const char *text)
{
    char url[256];
    snprintf(url, sizeof(url),
             "https://api.telegram.org/bot%s/sendMessage", sec.tg_token);

    char body[512];
    snprintf(body, sizeof(body), "chat_id=%s&text=%s", sec.tg_chat, text);

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    esp_http_client_set_header(cli, "Content-Type",
                               "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(cli, body, strlen(body));

    esp_err_t err = esp_http_client_perform(cli);
    if (err != ESP_OK) {
        ESP_LOGE("tg", "send failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(cli);
}

static void telegram_poll_task(void *arg)
{
    while (1) {
        char url[256];
        snprintf(url, sizeof(url),
                 "https://api.telegram.org/bot%s/getUpdates?offset=%lld&timeout=25",
                 sec.tg_token, (long long)(last_update_id + 1));

        esp_http_client_config_t cfg = {
            .url               = url,
            .method            = HTTP_METHOD_GET,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        esp_http_client_handle_t cli = esp_http_client_init(&cfg);
        esp_http_client_set_method(cli, HTTP_METHOD_GET);
        esp_err_t err = esp_http_client_open(cli, 0);
        if (err != ESP_OK) {
            ESP_LOGE("tg", "Failed to open HTTP connection: %s", esp_err_to_name(err));
        } else {
            int content_length = esp_http_client_fetch_headers(cli);
            if (content_length < 0) {
                ESP_LOGE("tg", "HTTP client fetch headers failed");
            } else {

                int data_read = esp_http_client_read_response(cli, output_buffer, MAX_HTTP_OUTPUT_BUFFER);
                if (data_read >= 0) {
                    ESP_LOGI("tg", "HTTP GET Status = %d, content_length = %d",
                    esp_http_client_get_status_code(cli),
                    esp_http_client_get_content_length(cli));
                    for(int i = 0; i < esp_http_client_get_content_length(cli); i++) {
                        putchar(output_buffer[i]);
                    }
                    putchar('\r');
                    putchar('\n');
                    struct tg_update update;
                    parse_telegram_response(output_buffer, &update);
                    if (update.update_id > last_update_id) {
                        last_update_id = update.update_id;
                    }
                    if (update.message_id > last_message_id) {
                        last_message_id = update.message_id;
                        ESP_LOGI("tg", "Received message: %s", update.text);
                        if (strcmp(update.text, "ok") == 0) {
                            char state_str[32];
                            state_to_string(get_state(), state_str, sizeof(state_str));
                            ESP_LOGI("tg", "Marking as taken %s", state_str);
                            if (get_state() == STATE_TAKEN) {
                                telegram_send("Already taken today!");
                            } else {
                                set_state(STATE_TAKEN);
                                telegram_send("Today's pills taken!");
                            }
                        } else {
                            telegram_send("Unrecognized command!");
                        }
                    }
                    
                } else {
                    ESP_LOGE("tg", "Failed to read response");
                }
            }
        }

        esp_http_client_cleanup(cli);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
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
    struct tm timeinfo = {0};
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

static void set_urgent_state()
{
    set_state(STATE_URGENT);
    ESP_LOGI("alarm", "21:00 — becomes urgent!");
    //telegram_send("test!");
}

void time_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(10000));
    while (1) {
        time_t now;
        struct tm ti;
        char hms[16];

        time(&now);
        localtime_r(&now, &ti);
        strftime(hms, sizeof(hms), "%H:%M:%S", &ti);
        /* If taken goes to Idle,
        *  if 21:00 goes to Urgent
        *
        */
        if (strcmp(hms, "23:59:00") >= 0) {
            set_state(STATE_IDLE);
        } else if (get_state() == STATE_IDLE && strcmp(hms, "21:00:00") >= 0) {
            set_urgent_state();
        }
        char state_str[32];
        state_to_string(get_state(), state_str, sizeof(state_str));
        oled_update(hms, state_str);
        vTaskDelay(pdMS_TO_TICKS(60000)); // Update every minute
    }
}


/* -----------------------------------------------------------------------
 * Entry point
 * ----------------------------------------------------------------------- */
void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());   // must come first

    ESP_ERROR_CHECK(secrets_load(&sec));
    oled_q = xQueueCreate(8, sizeof(oled_msg_t));
    xTaskCreate(oled_task, "oled", 4096, NULL, 3, NULL);

    // Init the state mutex before any tasks start using it
    state_mtx = xSemaphoreCreateMutex();
    set_state(STATE_INITIALIZING);
    oled_update("Initializing...", "Please wait");
    wifi_init_sta();           /* blocks until IP obtained */

    set_state(STATE_IDLE);  /* move to STATE_WIFI_CONNECTED */
    oled_update("Connected", "Please wait");
    time_init();
    xTaskCreate(time_task, "time", 2048, NULL, 1, NULL);

    xTaskCreate(telegram_poll_task, "tg_rx", 8192, NULL, 2, NULL);
}