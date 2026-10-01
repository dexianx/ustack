#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Event-driven socket API. Every call must be made from the thread that
 * owns the stack. Callbacks run inside packet processing; writes are
 * buffered and transmitted when the stack flushes.
 */

struct us_stack;
struct us_sock;

enum us_err {
    US_OK = 0,
    US_ERESET = 1,
    US_ETIMEDOUT = 2,
    US_EREFUSED = 3,
};

struct us_tcp_ops {
    void (*on_open)(struct us_sock *s);
    void (*on_readable)(struct us_sock *s);
    void (*on_writable)(struct us_sock *s);
    void (*on_close)(struct us_sock *s, enum us_err err);
};

int us_listen(struct us_stack *st, uint16_t port, const struct us_tcp_ops *ops, void *ctx);
struct us_sock *us_connect(struct us_stack *st, uint32_t addr, uint16_t port,
                           const struct us_tcp_ops *ops, void *ctx);

void *us_ctx(const struct us_sock *s);
void *us_data(const struct us_sock *s);
void us_set_data(struct us_sock *s, void *data);
struct us_stack *us_sock_stack(const struct us_sock *s);

size_t us_read(struct us_sock *s, void *buf, size_t n);
size_t us_readable(const struct us_sock *s);
bool us_eof(const struct us_sock *s);
size_t us_write(struct us_sock *s, const void *buf, size_t n);
size_t us_writable(const struct us_sock *s);
void us_set_nodelay(struct us_sock *s, bool on);
void us_shutdown(struct us_sock *s);
void us_close(struct us_sock *s);
void us_abort(struct us_sock *s);

typedef void (*us_udp_fn)(struct us_stack *st, void *ctx, uint32_t saddr, uint16_t sport,
                          uint16_t dport, const uint8_t *data, size_t len);

int us_udp_bind(struct us_stack *st, uint16_t port, us_udp_fn fn, void *ctx);
int us_udp_send(struct us_stack *st, uint16_t sport, uint32_t daddr, uint16_t dport,
                const void *data, size_t len);
