#ifndef __TYPES__
#define __TYPES__


typedef enum state {
    STATE_INITIALIZING = 0,
    STATE_IDLE,
    STATE_TAKEN,
    STATE_URGENT
} state_t;

typedef struct tg_user {
    std::string chat_id;
    state_t curr_state = STATE_INITIALIZING;
    int64_t last_update_id = 0;
    int64_t last_message_id = 0;
} tg_user_t;

typedef struct tg_update {
    int64_t update_id;
    int64_t message_id;
    char text[256];
    std::string chat_id;
} tg_update_t;

#endif