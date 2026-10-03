#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "stack.h"

struct tun_rx {
    size_t len;
    bool csum_ok;
};

int tun_open(const char *name, bool multi_queue, bool vnet);
int tun_enable_offload(int fd);
int tun_configure(const char *name, uint32_t addr, int prefix, uint16_t mtu, int txqlen);
ssize_t tun_recv(int fd, bool vnet, uint8_t *buf, size_t cap, struct tun_rx *rx);
ssize_t tun_send(int fd, bool vnet, const uint8_t *pkt, size_t len, const struct netif_tx *meta);
