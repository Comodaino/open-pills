idf_component_register(SRCS "tg_utils.cpp" "fsm.cpp" "secs.cpp" "main.cpp" 
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash esp_wifi esp_event esp_netif esp_http_client esp-tls esp_driver_gpio k0i05__esp_ssd1306)