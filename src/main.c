#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "apps/apps.h"
#include "core/util.h"
#include "netif/tun.h"
#include "stack.h"
#include "tcp/tcp.h"

#define MAX_SHARDS  64
#define RX_BUDGET   256
#define RX_BUF_SIZE 65536

struct options {
    const char *dev;
    uint32_t addr;
    uint32_t peer;
    int prefix;
    int shards;
    int txqlen;
    bool offload;
    bool pin;
    double drop_rx;
    double drop_tx;
    struct stack_config cfg;
};

struct shard {
    int id;
    int fd;
    bool vnet;
    pthread_t thread;
    struct us_stack *st;
    const struct options *opt;
    uint64_t rng;
    uint64_t dropped_rx;
    uint64_t dropped_tx;
    uint64_t tx_errors;
    uint8_t rxbuf[RX_BUF_SIZE] __attribute__((aligned(64)));
};

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * NSEC_PER_SEC + (uint64_t)ts.tv_nsec;
}

static void shard_tx(void *ctx, const uint8_t *pkt, size_t len, const struct netif_tx *meta)
{
    struct shard *sh = ctx;
    if (sh->opt->drop_tx > 0 && rand_unit(&sh->rng) < sh->opt->drop_tx) {
        sh->dropped_tx++;
        return;
    }
    if (tun_send(sh->fd, sh->vnet, pkt, len, meta) < 0)
        sh->tx_errors++;
}

static void *shard_main(void *arg)
{
    struct shard *sh = arg;
    struct us_stack *st = sh->st;
    struct pollfd pfd = { .fd = sh->fd, .events = POLLIN };

    if (sh->opt->pin) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(sh->id % CPU_SETSIZE, &set);
        pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    }

    while (!stop) {
        if (poll(&pfd, 1, st->wheel.armed ? 1 : 100) < 0 && errno != EINTR)
            break;
        st->now_ns = now_ns();
        for (int i = 0; i < RX_BUDGET; i++) {
            struct tun_rx rx;
            if (tun_recv(sh->fd, sh->vnet, sh->rxbuf, sizeof(sh->rxbuf), &rx) < 0)
                break;
            if (sh->opt->drop_rx > 0 && rand_unit(&sh->rng) < sh->opt->drop_rx) {
                sh->dropped_rx++;
                continue;
            }
            stack_input(st, sh->rxbuf, rx.len, rx.csum_ok);
        }
        stack_advance(st, now_ns());
    }
    return NULL;
}

static uint32_t parse_ip(const char *s)
{
    struct in_addr a;
    if (inet_pton(AF_INET, s, &a) != 1) {
        fprintf(stderr, "bad address: %s\n", s);
        exit(2);
    }
    return ntohl(a.s_addr);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "  --dev NAME        tun device (tun0)\n"
            "  --addr IP         stack address (10.0.0.2)\n"
            "  --peer IP/PREFIX  kernel-side address (10.0.0.1/24)\n"
            "  --shards N        worker shards / tun queues (1)\n"
            "  --mtu N           link MTU (1500)\n"
            "  --offload         enable vnet_hdr checksum + TSO/GSO offload\n"
            "  --pin             pin shard N to CPU N\n"
            "  --txqlen N        tun device queue length (4096)\n"
            "  --cc NAME         cubic | newreno (cubic)\n"
            "  --sndbuf BYTES    per-connection send buffer (1MiB)\n"
            "  --rcvbuf BYTES    per-connection receive buffer (1MiB)\n"
            "  --rto-min MS      minimum RTO (200)\n"
            "  --msl MS          maximum segment lifetime (30000)\n"
            "  --no-sack / --no-timestamps / --no-wscale\n"
            "  --drop-rx P --drop-tx P   inject random loss (0..1)\n",
            prog);
}

static void parse_args(int argc, char **argv, struct options *o)
{
    enum { O_DEV = 1, O_ADDR, O_PEER, O_SHARDS, O_MTU, O_OFFLOAD, O_PIN, O_CC, O_SNDBUF,
           O_RCVBUF, O_RTOMIN, O_MSL, O_NOSACK, O_NOTS, O_NOWS, O_DROPRX, O_DROPTX, O_TXQLEN };
    static const struct option longopts[] = {
        { "dev", required_argument, 0, O_DEV },       { "addr", required_argument, 0, O_ADDR },
        { "peer", required_argument, 0, O_PEER },     { "shards", required_argument, 0, O_SHARDS },
        { "mtu", required_argument, 0, O_MTU },       { "offload", no_argument, 0, O_OFFLOAD },
        { "pin", no_argument, 0, O_PIN },             { "cc", required_argument, 0, O_CC },
        { "sndbuf", required_argument, 0, O_SNDBUF }, { "rcvbuf", required_argument, 0, O_RCVBUF },
        { "rto-min", required_argument, 0, O_RTOMIN }, { "msl", required_argument, 0, O_MSL },
        { "no-sack", no_argument, 0, O_NOSACK },      { "no-timestamps", no_argument, 0, O_NOTS },
        { "no-wscale", no_argument, 0, O_NOWS },      { "drop-rx", required_argument, 0, O_DROPRX },
        { "drop-tx", required_argument, 0, O_DROPTX }, { "txqlen", required_argument, 0, O_TXQLEN },
        { "help", no_argument, 0, 'h' },
        { 0, 0, 0, 0 },
    };

    stack_config_defaults(&o->cfg);
    o->dev = "tun0";
    o->addr = o->cfg.addr;
    o->peer = 0x0a000001;
    o->prefix = 24;
    o->shards = 1;
    o->txqlen = 4096;

    int ch;
    while ((ch = getopt_long(argc, argv, "h", longopts, NULL)) != -1) {
        switch (ch) {
        case O_DEV: o->dev = optarg; break;
        case O_ADDR: o->addr = parse_ip(optarg); break;
        case O_PEER: {
            char *slash = strchr(optarg, '/');
            if (slash) {
                *slash = '\0';
                o->prefix = atoi(slash + 1);
            }
            o->peer = parse_ip(optarg);
            break;
        }
        case O_SHARDS: o->shards = atoi(optarg); break;
        case O_MTU: o->cfg.mtu = (uint16_t)atoi(optarg); break;
        case O_OFFLOAD: o->offload = true; break;
        case O_PIN: o->pin = true; break;
        case O_CC:
            if (!(o->cfg.cc = cc_find(optarg))) {
                fprintf(stderr, "unknown congestion control: %s\n", optarg);
                exit(2);
            }
            break;
        case O_SNDBUF: o->cfg.sndbuf = (uint32_t)strtoul(optarg, NULL, 0); break;
        case O_RCVBUF: o->cfg.rcvbuf = (uint32_t)strtoul(optarg, NULL, 0); break;
        case O_RTOMIN: o->cfg.rto_min_us = (uint32_t)atoi(optarg) * 1000; break;
        case O_MSL: o->cfg.msl_ms = (uint32_t)atoi(optarg); break;
        case O_NOSACK: o->cfg.sack = false; break;
        case O_NOTS: o->cfg.timestamps = false; break;
        case O_NOWS: o->cfg.wscale = false; break;
        case O_DROPRX: o->drop_rx = atof(optarg); break;
        case O_DROPTX: o->drop_tx = atof(optarg); break;
        case O_TXQLEN: o->txqlen = atoi(optarg); break;
        default: usage(argv[0]); exit(ch == 'h' ? 0 : 2);
        }
    }
    if (o->shards < 1 || o->shards > MAX_SHARDS) {
        fprintf(stderr, "--shards must be 1..%d\n", MAX_SHARDS);
        exit(2);
    }
    o->cfg.addr = o->addr;
}

static void print_stats(struct shard *shards, int n)
{
    struct stack_stats t = { 0 };
    uint64_t drop_rx = 0, drop_tx = 0, tx_err = 0;
    for (int i = 0; i < n; i++) {
        const uint64_t *src = (const uint64_t *)&shards[i].st->stats;
        uint64_t *dst = (uint64_t *)&t;
        for (size_t k = 0; k < sizeof(t) / sizeof(uint64_t); k++)
            dst[k] += src[k];
        drop_rx += shards[i].dropped_rx;
        drop_tx += shards[i].dropped_tx;
        tx_err += shards[i].tx_errors;
    }
    fprintf(stderr,
            "ustack: rx %" PRIu64 " pkts / %" PRIu64 " B, tx %" PRIu64 " pkts / %" PRIu64 " B\n"
            "  tcp: rx %" PRIu64 " tx %" PRIu64 " retrans %" PRIu64 " rto %" PRIu64 " tlp %" PRIu64
            " recoveries %" PRIu64 " ooo %" PRIu64 " opened %" PRIu64 " closed %" PRIu64
            " bad %" PRIu64 " paws %" PRIu64 " dup %" PRIu64 " challenge %" PRIu64 "\n"
            "  ip bad %" PRIu64 " icmp echo %" PRIu64 " udp rx %" PRIu64
            " injected drops rx %" PRIu64 " tx %" PRIu64 " tx errors %" PRIu64 "\n",
            t.rx_packets, t.rx_bytes, t.tx_packets, t.tx_bytes, t.tcp_rx_segs, t.tcp_tx_segs,
            t.tcp_retrans, t.tcp_rto, t.tcp_tlp, t.tcp_recoveries, t.tcp_ooo, t.tcp_opened,
            t.tcp_closed, t.tcp_bad, t.tcp_paws, t.tcp_dup, t.tcp_challenge_ack, t.ip_bad, t.icmp_echo, t.udp_rx, drop_rx, drop_tx, tx_err);
}

int main(int argc, char **argv)
{
    static struct options opt;
    parse_args(argc, argv, &opt);

    struct shard *shards = aligned_alloc(64, sizeof(struct shard) * (size_t)opt.shards);
    if (!shards)
        return 1;
    memset(shards, 0, sizeof(struct shard) * (size_t)opt.shards);

    bool mq = opt.shards > 1;
    for (int i = 0; i < opt.shards; i++) {
        struct shard *sh = &shards[i];
        sh->id = i;
        sh->opt = &opt;
        sh->vnet = opt.offload;
        sh->rng = opt.cfg.seed * 0x9e3779b97f4a7c15ull + (uint64_t)i + 1;
        sh->fd = tun_open(opt.dev, mq, sh->vnet);
        if (sh->fd < 0) {
            perror("tun_open");
            return 1;
        }
        if (opt.offload && tun_enable_offload(sh->fd) < 0) {
            perror("TUNSETOFFLOAD");
            return 1;
        }
        struct stack_config cfg = opt.cfg;
        cfg.seed = opt.cfg.seed + (uint64_t)i;
        struct netif nif = { .tx = shard_tx, .ctx = sh, .offload = opt.offload };
        sh->st = stack_create(&cfg, &nif, now_ns());
        if (!sh->st) {
            fprintf(stderr, "stack_create failed\n");
            return 1;
        }
        apps_register(sh->st);
    }
    if (tun_configure(opt.dev, opt.peer, opt.prefix, opt.cfg.mtu, opt.txqlen) < 0) {
        perror("tun_configure");
        return 1;
    }

    struct in_addr a = { htonl(opt.addr) }, p = { htonl(opt.peer) };
    char abuf[INET_ADDRSTRLEN], pbuf[INET_ADDRSTRLEN];
    fprintf(stderr, "ustack: %s up, stack %s, kernel %s/%d, mtu %u, %d shard(s), cc %s%s\n",
            opt.dev, inet_ntop(AF_INET, &a, abuf, sizeof(abuf)),
            inet_ntop(AF_INET, &p, pbuf, sizeof(pbuf)), opt.prefix, opt.cfg.mtu, opt.shards,
            opt.cfg.cc->name, opt.offload ? ", offload" : "");

    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    for (int i = 0; i < opt.shards; i++)
        pthread_create(&shards[i].thread, NULL, shard_main, &shards[i]);
    for (int i = 0; i < opt.shards; i++)
        pthread_join(shards[i].thread, NULL);

    print_stats(shards, opt.shards);
    for (int i = 0; i < opt.shards; i++) {
        stack_destroy(shards[i].st);
        close(shards[i].fd);
    }
    free(shards);
    return 0;
}
