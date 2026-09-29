#include"tg_utils.hpp"

int add_user(std::string id) {
    auto it = std::find_if(users.begin(), users.end(), [&id](const tg_user_t& p) {
        return p.chat_id == id;
    });
    if (it != users.end()) {
        return 1;
    }
    tg_user_t new_user;
    new_user.chat_id = id;
    users.push_front(new_user);
    return 0;
}


int rm_user(std::string id) {
    auto it = std::find_if(users.begin(), users.end(), [&id](const tg_user_t& p) {
        return p.chat_id == id;
    });
    if (it == users.end()) {
        return 1;
    }
    users.erase(it);
    return 0;
}



int get_user(std::string id, tg_user_t* usr) {
    auto it = std::find_if(users.begin(), users.end(), [&id](const tg_user_t& p) {
        return p.chat_id == id;
    });
    if (it == users.end()) {
        return 1;
    }

    usr->chat_id = it->chat_id;
    usr->curr_state = it->curr_state;
    usr->last_update_id = it->last_update_id;
    usr->last_message_id = it->last_message_id;
    return 0;

}

void parse_telegram_response(const char *json, tg_update_t *update)
{    /* This is a very naive parser just for demonstration.  In production,
     * consider using a proper JSON library like cJSON or jsmn. */
    const char *update_id_str = "\"update_id\":";
    const char *message_id_str = "\"message_id\":";
    const char *chat_str = "\"chat\":";
    const char *id_str = "\"id\":";
    const char *text_str = "\"text\":\"";

    char *update_id_pos = strstr(json, update_id_str);
    char *message_id_pos = strstr(json, message_id_str);
    char *chat_pos = strstr(json, chat_str);
    char *text_pos = strstr(json, text_str);

    if (update_id_pos) {
        update->update_id = atoll(update_id_pos + strlen(update_id_str));
    }
    
    if (message_id_pos) {
        update->message_id = atoll(message_id_pos + strlen(message_id_str));
    }
    
    // Extract the chat ID
    if (chat_pos) {
        // Look for the "id": field specifically after the "chat": object starts
        char *chat_id_pos = strstr(chat_pos, id_str);
        if (chat_id_pos) {
            chat_id_pos += strlen(id_str);
            
            // Skip any potential whitespace
            while (*chat_id_pos == ' ') chat_id_pos++;
            
            char temp_id[32] = {0};
            int i = 0;
            
            // Telegram Chat IDs can be positive (users) or negative (groups)
            while ((chat_id_pos[i] == '-' || (chat_id_pos[i] >= '0' && chat_id_pos[i] <= '9')) && i < sizeof(temp_id) - 1) {
                temp_id[i] = chat_id_pos[i];
                i++;
            }
            temp_id[i] = '\0';
            
            // Assuming your tg_update_t struct uses a char array (e.g., char chat_id[32];)
            update->chat_id = temp_id;
        }
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

void print_users() {
    ESP_LOGI("users", "--- Current User List (Total: %d) ---", users.size());
    
    int index = 1;
    for (const auto& u : users) {
        ESP_LOGI("users", "%d. Chat ID: %s | State: %d | Last Update: %lld | Last Msg: %lld", 
                 index++,
                 u.chat_id.c_str(), 
                 (int)u.curr_state, 
                 (long long)u.last_update_id, 
                 (long long)u.last_message_id);
    }
    
    ESP_LOGI("users", "-----------------------------------");
}

void telegram_send(const char *text, std::string user_id)
{
    char url[256];
    snprintf(url, sizeof(url),
             "https://api.telegram.org/bot%s/sendMessage", sec.tg_token);

    char body[512];
    snprintf(body, sizeof(body), "chat_id=%s&text=%s", user_id.c_str(), text);
    esp_http_client_config_t cfg = {};
    cfg.url               = url;
    cfg.method            = HTTP_METHOD_POST;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    esp_http_client_set_header(cli, "Content-Type",
                               "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(cli, body, strlen(body));
    xSemaphoreTake(tg_send_mtx, portMAX_DELAY);
    
    esp_err_t err = esp_http_client_perform(cli);
    if (err != ESP_OK) {
        ESP_LOGE("tg", "send failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(cli);
    xSemaphoreGive(tg_send_mtx);
}


void telegram_poll_task(void *arg)
{
    // Track the highest update_id across ALL users to acknowledge messages
    int64_t global_update_id = 0; 

    while (1) {
        char url[256];
        // Request updates starting from the next unread global message
        snprintf(url, sizeof(url),
                "https://api.telegram.org/bot%s/getUpdates?offset=%lld&timeout=25",
                sec.tg_token, (long long)(global_update_id + 1));

        esp_http_client_config_t cfg = {};
        cfg.url               = url;
        cfg.method            = HTTP_METHOD_GET;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
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
                    // Null-terminate the buffer safely
                    output_buffer[data_read] = '\0';
                    
                    tg_update_t update = {}; // Clear struct before parsing
                    parse_telegram_response(output_buffer, &update);
                    
                    // If we successfully parsed a valid update
                    if (update.update_id > 0) {
                        // 1. Advance the global offset so we don't process this update again
                        if (update.update_id > global_update_id) {
                            global_update_id = update.update_id;
                        }

                        std::string incoming_chat_id(update.chat_id);
                        
                        if (!incoming_chat_id.empty()) {
                            ESP_LOGI("tg", "Received message: '%s' from chat: %s", update.text, incoming_chat_id.c_str());

                            // 2. Handle the start command to register a new user
                            if (strcmp(update.text, "/start") == 0 || strcmp(update.text, "\\start") == 0) {
                                if (add_user(incoming_chat_id) == 0) {
                                    telegram_send("Welcome! You are now registered. Send 'ok' to confirm pills.", incoming_chat_id);
                                    ESP_LOGI("tg", "New user registered: %s", incoming_chat_id.c_str());
                                } else {
                                    telegram_send("You are already registered!", incoming_chat_id);
                                }
                            } 
                            // 3. Process commands for existing users
                            else {
                                tg_user_t current_user = {};
                                ESP_LOGI("tg", "test");
                                if (get_user(incoming_chat_id, &current_user) == 0) {
                                    // User is registered, process their command
                                    ESP_LOGI("tg", "user is registered");

                                    if (strcmp(update.text, "ok") == 0) {
                                        ESP_LOGI("tg", "pill checking");
                                        if (current_user.curr_state == STATE_TAKEN) {
                                            ESP_LOGI("tg", "pill taken");
                                            telegram_send("Already taken today!", incoming_chat_id);

                                        } else {
                                            ESP_LOGI("tg", "pill not taken");
                                            set_state(STATE_TAKEN, incoming_chat_id);
                                            ESP_LOGI("tg", "pill is now taken");
                                            telegram_send("Today's pills taken!", incoming_chat_id);
                                        }
                                    } else {
                                        telegram_send("Unrecognized command!", incoming_chat_id);
                                    }
                                } else {
                                    // User is NOT registered
                                    telegram_send("You are not registered yet. Send /start to register.", incoming_chat_id);
                                }
                            }
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