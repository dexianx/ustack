#pragma once

#include <stddef.h>

#include "ustack.h"

#define APP_HTTP_PORT    80
#define APP_ECHO_PORT    7
#define APP_DISCARD_PORT 9

void apps_register(struct us_stack *st);
int http_listen(struct us_stack *st, uint16_t port);
int echo_listen(struct us_stack *st, uint16_t port);
int discard_listen(struct us_stack *st, uint16_t port);
size_t stack_stats_json(const struct us_stack *st, char *buf, size_t cap);
