/* RDNSS lifetime/validation regression using real router-advertisement bytes. */
#include "../../userland/apps/rdnssd/rdnss.h"
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    struct rdnss_cache cache = {0}, before;
    struct in6_addr router;
    unsigned char ra[40] = {134, 0};
    char text[256];
    assert(inet_pton(AF_INET6, "fe80::1", &router) == 1);
    ra[16] = 25; ra[17] = 3; ra[23] = 90;
    assert(inet_pton(AF_INET6, "2001:db8::53", ra + 24) == 1);
    assert(rdnss_receive(&cache, &router, 2, 255, ra, sizeof ra, 1000) == 1);
    assert(rdnss_format(&cache, 1000, text, sizeof text) >= 0);
    assert(!strcmp(text, "2001:db8::53\n"));
    before = cache;
    ra[17] = 0;
    assert(rdnss_receive(&cache, &router, 2, 255, ra, sizeof ra, 1000) < 0);
    assert(!memcmp(&before, &cache, sizeof cache));
    ra[17] = 3;
    assert(rdnss_receive(&cache, &router, 2, 254, ra, sizeof ra, 1000) < 0);
    assert(!memcmp(&before, &cache, sizeof cache));
    assert(rdnss_format(&cache, 91000, text, sizeof text) == 0);
    assert(!text[0]);
    assert(inet_pton(AF_INET6, "fe80::53", ra + 24) == 1);
    assert(rdnss_receive(&cache, &router, 2, 255, ra, sizeof ra, 2000) == 1);
    assert(rdnss_format(&cache, 2000, text, sizeof text) >= 0);
    assert(!strcmp(text, "fe80::53%2\n2001:db8::53\n"));
    ra[23] = 0;
    assert(rdnss_receive(&cache, &router, 2, 255, ra, sizeof ra, 3000) == 1);
    assert(rdnss_format(&cache, 3000, text, sizeof text) >= 0);
    assert(!strcmp(text, "2001:db8::53\n"));
    assert(rdnss_server_valid("2001:db8::53"));
    assert(rdnss_server_valid("fe80::53%2"));
    assert(rdnss_server_valid("::1"));
    assert(!rdnss_server_valid("fe80::53"));
    assert(!rdnss_server_valid("::"));
    assert(!rdnss_server_valid("ff02::1"));
    assert(!rdnss_server_valid("2001:db8::53;id"));
    puts("PASS RDNSS RA validation, scoped DNS, lifetimes and withdrawal");
    return 0;
}
