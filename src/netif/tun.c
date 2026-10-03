#include "netif/tun.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <linux/virtio_net.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include "net/proto.h"

int tun_open(const char *name, bool multi_queue, bool vnet)
{
    int fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -1;
    struct ifreq ifr = { 0 };
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    if (multi_queue)
        ifr.ifr_flags |= IFF_MULTI_QUEUE;
    if (vnet)
        ifr.ifr_flags |= IFF_VNET_HDR;
    strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int tun_enable_offload(int fd)
{
    return ioctl(fd, TUNSETOFFLOAD, TUN_F_CSUM | TUN_F_TSO4);
}

static int ifreq_ioctl(int sock, unsigned long req, struct ifreq *ifr)
{
    return ioctl(sock, req, ifr) < 0 ? -1 : 0;
}

static void set_sockaddr(struct sockaddr *sa, uint32_t addr)
{
    struct sockaddr_in sin = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(addr) };
    memcpy(sa, &sin, sizeof(sin));
}

int tun_configure(const char *name, uint32_t addr, int prefix, uint16_t mtu, int txqlen)
{
    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sock < 0)
        return -1;
    struct ifreq ifr = { 0 };
    strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
    int rc = 0;

    set_sockaddr(&ifr.ifr_addr, addr);
    rc |= ifreq_ioctl(sock, SIOCSIFADDR, &ifr);
    set_sockaddr(&ifr.ifr_netmask, prefix ? ~0u << (32 - prefix) : 0);
    rc |= ifreq_ioctl(sock, SIOCSIFNETMASK, &ifr);
    ifr.ifr_mtu = mtu;
    rc |= ifreq_ioctl(sock, SIOCSIFMTU, &ifr);
    ifr.ifr_qlen = txqlen;
    rc |= ifreq_ioctl(sock, SIOCSIFTXQLEN, &ifr);
    rc |= ifreq_ioctl(sock, SIOCGIFFLAGS, &ifr);
    ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
    rc |= ifreq_ioctl(sock, SIOCSIFFLAGS, &ifr);
    close(sock);
    return rc;
}

ssize_t tun_recv(int fd, bool vnet, uint8_t *buf, size_t cap, struct tun_rx *rx)
{
    struct virtio_net_hdr vh = { 0 };
    struct iovec iov[2] = { { &vh, sizeof(vh) }, { buf, cap } };
    ssize_t n = vnet ? readv(fd, iov, 2) : read(fd, buf, cap);
    if (n < 0)
        return -1;
    if (vnet) {
        if ((size_t)n < sizeof(vh)) {
            errno = EINVAL;
            return -1;
        }
        n -= (ssize_t)sizeof(vh);
    }
    rx->len = (size_t)n;
    rx->csum_ok = vnet && (vh.flags & (VIRTIO_NET_HDR_F_NEEDS_CSUM | VIRTIO_NET_HDR_F_DATA_VALID));
    return n;
}

ssize_t tun_send(int fd, bool vnet, const uint8_t *pkt, size_t len, const struct netif_tx *meta)
{
    if (!vnet)
        return write(fd, pkt, len);

    struct virtio_net_hdr vh = { 0 };
    if (meta && meta->csum_partial) {
        const struct tcp_hdr *th = (const struct tcp_hdr *)(pkt + meta->csum_start);
        vh.flags = VIRTIO_NET_HDR_F_NEEDS_CSUM;
        vh.csum_start = meta->csum_start;
        vh.csum_offset = meta->csum_offset;
        vh.hdr_len = (uint16_t)(meta->csum_start + (th->doff >> 4) * 4);
        if (meta->gso_size) {
            vh.gso_type = VIRTIO_NET_HDR_GSO_TCPV4;
            vh.gso_size = meta->gso_size;
        }
    }
    struct iovec iov[2] = { { &vh, sizeof(vh) }, { (void *)pkt, len } };
    return writev(fd, iov, 2);
}
