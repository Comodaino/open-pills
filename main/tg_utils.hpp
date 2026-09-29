
/* -----------------------------------------------------------------------
 * Telegram
 * ----------------------------------------------------------------------- */
#ifndef __TG_UTILS__
#define __TG_UTILS__

#include "fsm.hpp"
#include "secs.hpp"
#include "types.hpp"
#include <string>
#include <list>
#include <algorithm>
#define MAX_HTTP_OUTPUT_BUFFER 2048

extern char output_buffer[MAX_HTTP_OUTPUT_BUFFER];

extern std::list<tg_user_t> users;
extern SemaphoreHandle_t tg_send_mtx;

int add_user(std::string id);
int rm_user(std::string id);
int get_user(std::string id, tg_user_t* usr);

void print_users();

void parse_telegram_response(const char *json, struct tg_update *update);
void telegram_send(const char *text, std::string user_id);

void telegram_poll_task(void *arg);

#endif