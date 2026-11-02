/* SPDX-License-Identifier: Apache-2.0 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "rdnss.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;
static void stop(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static uint64_t monotonic_ms(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) < 0) {
        perror("rdnssd: clock_gettime");
        exit(1);
    }
    return (uint64_t)time.tv_sec * 1000 + (unsigned long)time.tv_nsec / 1000000;
}

static int publish(const char *text)
{
    char temporary[] = "/run/reliefos/.ra-dns.XXXXXX";
    int fd = mkstemp(temporary);
    if (fd < 0)
        return -1;
    size_t remaining = strlen(text);
    const char *next = text;
    int failed = fchmod(fd, 0644);
    while (!failed && remaining) {
        ssize_t count = write(fd, next, remaining);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            failed = -1;
            break;
        }
        remaining -= (size_t)count;
        next += count;
    }
    if (close(fd) < 0)
        failed = -1;
    if (!failed && rename(temporary, "/run/reliefos/ra-dns") < 0)
        failed = -1;
    unlink(temporary);
    if (failed)
        return -1;
    pid_t child = fork();
    if (child < 0)
        return -1;
    if (!child) {
        execl("/usr/lib/reliefos/publish-resolver", "publish-resolver", (char *)NULL);
        _exit(127);
    }
    int status;
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR)
            return -1;
    return WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -1;
}

static int receive_ra(int fd, unsigned interface, struct rdnss_cache *cache)
{
    unsigned char packet[65535];
    union { struct cmsghdr align; unsigned char bytes[128]; } control;
    struct sockaddr_in6 source;
    struct iovec iov = {.iov_base = packet, .iov_len = sizeof packet};
    struct msghdr message = {.msg_name = &source, .msg_namelen = sizeof source,
        .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control.bytes,
        .msg_controllen = sizeof control.bytes};
    ssize_t length = recvmsg(fd, &message, 0);
    if (length < 0)
        return errno == EINTR || errno == EAGAIN ? 0 : -1;
    if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
        message.msg_namelen < sizeof source || source.sin6_family != AF_INET6)
        return 0;
    unsigned ifindex = 0;
    int hops = -1;
    for (struct cmsghdr *header = CMSG_FIRSTHDR(&message); header;
         header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level != IPPROTO_IPV6)
            continue;
        if (header->cmsg_type == IPV6_PKTINFO &&
            header->cmsg_len >= CMSG_LEN(sizeof(struct in6_pktinfo))) {
            struct in6_pktinfo info;
            memcpy(&info, CMSG_DATA(header), sizeof info);
            ifindex = info.ipi6_ifindex;
        } else if (header->cmsg_type == IPV6_HOPLIMIT &&
                   header->cmsg_len >= CMSG_LEN(sizeof hops))
            memcpy(&hops, CMSG_DATA(header), sizeof hops);
    }
    if (ifindex != interface)
        return 0;
    int result = rdnss_receive(cache, &source.sin6_addr, ifindex, hops,
                               packet, (size_t)length, monotonic_ms());
    if (result == -ENOSPC)
        fputs("rdnssd: RA DNS cache is full\n", stderr);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--validate-server"))
        return rdnss_server_valid(argv[2]) ? 0 : 1;
    if (argc > 2 || (argc == 2 && argv[1][0] == '-')) {
        fputs("Usage: rdnssd [interface]\n       rdnssd --validate-server IPv6-address\n", stderr);
        return 2;
    }
    const char *interface = argc == 2 ? argv[1] : "eth0";
    unsigned ifindex = if_nametoindex(interface);
    if (!ifindex) {
        perror("rdnssd: interface");
        return 1;
    }
    int fd = socket(AF_INET6, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, IPPROTO_ICMPV6);
    int one = 1, zero = 0, hops = 255;
    if (fd < 0 ||
        setsockopt(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &one, sizeof one) < 0 ||
        setsockopt(fd, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &one, sizeof one) < 0 ||
        setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &ifindex, sizeof ifindex) < 0 ||
        setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof hops) < 0 ||
        setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &zero, sizeof zero) < 0) {
        perror("rdnssd: IPv6 socket");
        if (fd >= 0)
            close(fd);
        return 1;
    }
    struct sigaction action = {.sa_handler = stop};
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    struct rdnss_cache cache = {0};
    char previous[256] = "invalid", current[256];
    unsigned solicitations = 0;
    uint64_t solicit_at = monotonic_ms() + 1000;
    int was_up = -1;
    int result = 0;
    while (!stopping) {
        uint64_t now = monotonic_ms();
        struct ifreq link = {0};
        if (strlen(interface) >= sizeof link.ifr_name) {
            result = 1;
            break;
        }
        strcpy(link.ifr_name, interface);
        int up = ioctl(fd, SIOCGIFFLAGS, &link) == 0 && (link.ifr_flags & IFF_UP);
        if (up != was_up) {
            memset(&cache, 0, sizeof cache);
            solicitations = 0;
            solicit_at = now + 1000;
            was_up = up;
        }
        if (rdnss_format(&cache, now, current, sizeof current) < 0) {
            fputs("rdnssd: cannot format DNS state\n", stderr);
            result = 1;
            break;
        }
        if (strcmp(previous, current) && !publish(current))
            strcpy(previous, current);
        if (up && solicitations < 3 && now >= solicit_at) {
            struct sockaddr_in6 target = {.sin6_family = AF_INET6,
                .sin6_scope_id = ifindex};
            inet_pton(AF_INET6, "ff02::2", &target.sin6_addr);
            const unsigned char rs[8] = {133, 0};
            if (sendto(fd, rs, sizeof rs, 0, (struct sockaddr *)&target,
                       sizeof target) == (ssize_t)sizeof rs)
                ++solicitations;
            solicit_at = now + 4000;
        }
        /* One idle wake per second also retires DNS when the link is disabled. */
        uint64_t next = now + 1000;
        if (up && solicitations < 3 && solicit_at < next)
            next = solicit_at;
        for (unsigned i = 0; i < 64; ++i)
            if (cache.entries[i].expires > now && cache.entries[i].expires < next)
                next = cache.entries[i].expires;
        struct pollfd wait = {.fd = fd, .events = POLLIN};
        int ready = poll(&wait, 1, (int)(next - now));
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0 || (ready && (wait.revents & (POLLERR | POLLHUP | POLLNVAL))) ||
            (ready && receive_ra(fd, ifindex, &cache) < 0)) {
            perror("rdnssd: receive");
            result = 1;
            break;
        }
    }
    close(fd);
    if (publish("") < 0) {
        fputs("rdnssd: cannot clear RA DNS state\n", stderr);
        result = 1;
    }
    return result;
}
