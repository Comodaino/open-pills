# Wi-Fi Pill Reminder

An ESP32 pill reminder that tracks daily medication via a Telegram bot and shows status on an SSD1306 OLED display.

## Libraries

### ESP-IDF components
| Component | Use |
|---|---|
| `esp_wifi` | Wi-Fi STA connection and event handling |
| `esp_http_client` | HTTPS requests to the Telegram Bot API |
| `esp_crt_bundle` | TLS certificate bundle for HTTPS |
| `esp_netif` | Network interface abstraction |
| `esp_event` | System event loop |
| `esp_sntp` | NTP time sync (`pool.ntp.org`) |
| `nvs_flash` / `nvs` | Non-volatile storage for credentials |
| `driver/i2c_master` | I2C bus driver for the OLED |
| `driver/gpio` | OLED reset pin control |

### FreeRTOS
| API | Use |
|---|---|
| `xTaskCreate` | OLED, time, and Telegram polling tasks |
| `xQueueCreate` / `xQueueSend` / `xQueueReceive` | Thread-safe OLED message queue |
| `xSemaphoreCreateMutex` | Protects the state machine across tasks |
| `xEventGroupCreate` | Signals Wi-Fi connection ready |

### Third-party
| Library | Use |
|---|---|
| [`k0i05/esp_ssd1306`](https://components.espressif.com/components/k0i05/esp_ssd1306) | SSD1306 128×64 OLED display driver |

## Build & flash

```sh
idf.py add-dependency "k0i05/esp_ssd1306"
idf.py build
idf.py flash monitor
```

Credentials (`wifi_ssid`, `wifi_pass`, `tg_token`, `tg_chat`) must be provisioned into the `secrets` NVS namespace before first boot.
