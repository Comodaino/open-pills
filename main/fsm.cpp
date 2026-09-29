#include "fsm.hpp"

void state_to_string(state_t s, char *buf, size_t buf_size) {
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

void set_state(state_t new_state, std::string id) {
    xSemaphoreTake(state_mtx, portMAX_DELAY);
    auto it = std::find_if(users.begin(), users.end(), [&id](const tg_user_t& p) {
            return p.chat_id == id;
        });
        
    // If the user is found, update their state in the list
    if (it != users.end()) {
        it->curr_state = new_state;
    }
    xSemaphoreGive(state_mtx);
}

void next_state(std::string id) {
    xSemaphoreTake(state_mtx, portMAX_DELAY);
    tg_user_t* user = {};
    get_user(id, user);
    
    switch (user->curr_state) {
        case STATE_INITIALIZING: user->curr_state = STATE_IDLE;
        break;
        case STATE_IDLE:         user->curr_state = STATE_TAKEN;
        break;
        case STATE_TAKEN:        user->curr_state = STATE_URGENT;
        break;
        case STATE_URGENT:       user->curr_state = STATE_INITIALIZING;
        break;
    };
    xSemaphoreGive(state_mtx);
}