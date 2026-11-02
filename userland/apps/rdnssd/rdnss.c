/* SPDX-License-Identifier: Apache-2.0 */
#include "rdnss.h"
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>

static uint32_t read32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | p[3];
}

static int unicast(const struct in6_addr *address)
{
    return !IN6_IS_ADDR_UNSPECIFIED(address) && !IN6_IS_ADDR_MULTICAST(address);
}

int rdnss_server_valid(const char *text)
{
    struct in6_addr address;
    char numeric[INET6_ADDRSTRLEN];
    const char *zone = strchr(text, '%');
    size_t length = zone ? (size_t)(zone - text) : strlen(text);
    if (!length || length >= sizeof numeric)
        return 0;
    memcpy(numeric, text, length);
    numeric[length] = 0;
    if (inet_pton(AF_INET6, numeric, &address) != 1 || !unicast(&address))
        return 0;
    if (!IN6_IS_ADDR_LINKLOCAL(&address))
        return !zone;
    if (!zone || !zone[1])
        return 0;
    /* Numeric zones work even before libc's interface-name cache is ready. */
    uint64_t index = 0;
    const char *p = zone + 1;
    for (; *p >= '0' && *p <= '9'; ++p) {
        index = index * 10 + (unsigned)(*p - '0');
        if (index > UINT_MAX)
            return 0;
    }
    if (!*p)
        return index != 0;
    if (strlen(zone + 1) >= IF_NAMESIZE)
        return 0;
    for (p = zone + 1; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.'))
            return 0;
    return if_nametoindex(zone + 1) != 0;
}

int rdnss_receive(struct rdnss_cache *cache, const struct in6_addr *router,
                  unsigned ifindex, int hops, const void *bytes, size_t length,
                  uint64_t now)
{
    const unsigned char *packet = bytes;
    if (!ifindex || hops != 255 || !IN6_IS_ADDR_LINKLOCAL(router) ||
        length < 16 || packet[0] != 134 || packet[1])
        return -EINVAL;
    /* Validate the entire option stream before applying any lifetime update. */
    for (size_t at = 16; at < length;) {
        if (length - at < 2 || !packet[at + 1])
            return -EINVAL;
        size_t option = (size_t)packet[at + 1] * 8;
        if (option > length - at ||
            (packet[at] == 25 && (option < 24 || (option - 8) % 16)))
            return -EINVAL;
        at += option;
    }
    struct rdnss_cache staged = *cache;
    for (unsigned i = 0; i < 64; ++i)
        if (staged.entries[i].expires <= now)
            memset(&staged.entries[i], 0, sizeof staged.entries[i]);
    int changed = 0;
    for (size_t at = 16; at < length; at += (size_t)packet[at + 1] * 8) {
        if (packet[at] != 25)
            continue;
        uint32_t lifetime = read32(packet + at + 4);
        size_t end = at + (size_t)packet[at + 1] * 8;
        for (size_t offset = at + 8; offset < end; offset += 16) {
            struct in6_addr server;
            memcpy(&server, packet + offset, sizeof server);
            if (!unicast(&server))
                continue;
            struct rdnss_entry *slot = NULL;
            for (unsigned i = 0; i < 64; ++i) {
                struct rdnss_entry *entry = &staged.entries[i];
                if (entry->expires && entry->ifindex == ifindex &&
                    !memcmp(&entry->router, router, 16) &&
                    !memcmp(&entry->server, &server, 16)) {
                    slot = entry;
                    break;
                }
            }
            if (!lifetime) {
                if (slot) {
                    memset(slot, 0, sizeof *slot);
                    changed = 1;
                }
                continue;
            }
            if (!slot)
                for (unsigned i = 0; i < 64; ++i)
                    if (!staged.entries[i].expires) {
                        slot = &staged.entries[i];
                        break;
                    }
            if (!slot)
                return -ENOSPC;
            slot->router = *router;
            slot->server = server;
            slot->ifindex = ifindex;
            slot->expires = lifetime == UINT32_MAX ? UINT64_MAX :
                            now + (uint64_t)lifetime * 1000;
            slot->sequence = ++staged.sequence;
            changed = 1;
        }
    }
    *cache = staged;
    return changed;
}

int rdnss_format(const struct rdnss_cache *cache, uint64_t now,
                 char *output, size_t capacity)
{
    if (!capacity)
        return -ENOSPC;
    output[0] = 0;
    size_t used = 0;
    uint64_t before = UINT64_MAX;
    unsigned emitted = 0;
    while (emitted < 3) {
        const struct rdnss_entry *best = NULL;
        for (unsigned i = 0; i < 64; ++i) {
            const struct rdnss_entry *entry = &cache->entries[i];
            if (entry->expires > now && entry->sequence < before &&
                (!best || entry->sequence > best->sequence))
                best = entry;
        }
        if (!best)
            break;
        before = best->sequence;
        char address[INET6_ADDRSTRLEN], scoped[INET6_ADDRSTRLEN + 16];
        if (!inet_ntop(AF_INET6, &best->server, address, sizeof address))
            return -errno;
        int size = IN6_IS_ADDR_LINKLOCAL(&best->server) ?
            snprintf(scoped, sizeof scoped, "%s%%%u\n", address, best->ifindex) :
            snprintf(scoped, sizeof scoped, "%s\n", address);
        if (size < 0 || (size_t)size >= sizeof scoped)
            return -ENOSPC;
        /* Each complete line has a newline, so substring matches are unambiguous. */
        const char *line = output;
        int duplicate = 0;
        while (*line) {
            if (!strncmp(line, scoped, (size_t)size)) {
                duplicate = 1;
                break;
            }
            line = strchr(line, '\n') + 1;
        }
        if (duplicate)
            continue;
        if ((size_t)size >= capacity - used)
            return -ENOSPC;
        memcpy(output + used, scoped, (size_t)size + 1);
        used += (size_t)size;
        ++emitted;
    }
    return (int)used;
}
