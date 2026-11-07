/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFOS_RDNSS_H
#define RELIEFOS_RDNSS_H
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

/* Bounded userspace cache; keep alternate routers until their own expiry. */
struct rdnss_entry {
    struct in6_addr router, server;
    unsigned ifindex;
    uint64_t expires, sequence;
};
struct rdnss_cache {
    struct rdnss_entry entries[64];
    uint64_t sequence;
};

/* Accept only complete, locally received RAs with RFC 4861 scope/hop checks. */
int rdnss_receive(struct rdnss_cache *, const struct in6_addr *, unsigned,
                  int hops, const void *, size_t, uint64_t now);
/* Emit at most three unique servers, including a zone for link-local DNS. */
int rdnss_format(const struct rdnss_cache *, uint64_t, char *, size_t);
int rdnss_server_valid(const char *);
#endif
