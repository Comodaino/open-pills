#include"secs.hpp"

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

    nvs_close(h);
    ESP_LOGI("secrets", "Secrets loaded OK");
    return ESP_OK;
}