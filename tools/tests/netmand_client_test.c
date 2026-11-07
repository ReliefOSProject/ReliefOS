/* Historical test filename; exercise the direct adapter, with no daemon. */
#define _GNU_SOURCE
#include <assert.h>
#include "../../kernel/reliefnt/tools/tests/host_tmp.h"
#include <stdarg.h>
#include <errno.h>
#include <reliefos/net_control.h>
#include <fcntl.h>
int lease_test_open(const char *path, int flags, ...);
#define open lease_test_open
#include "../../userland/runtime/src/netsock.c"
#undef open
static const char *lease_test_path;
int lease_test_open(const char *path, int flags, ...)
{
    if (!strcmp(path, "/run/reliefos/dhcp-lease")) {
        if (!lease_test_path) { errno = ENOENT; return -1; }
        return open(lease_test_path, flags);
    }
    errno = EACCES; return -1;
}
static int operations, failure;
int ioctl(int fd, unsigned long op, ...)
{
    (void)fd;
    assert(op == RELIEFOS_NET_CONTROL_IOCTL);
    va_list args; va_start(args, op);
    struct reliefos_net_control *control = va_arg(args, struct reliefos_net_control *);
    va_end(args);
    ++operations;
    assert(control->version == RELIEFOS_NET_CONTROL_VERSION);
    if (failure) { errno = ENETDOWN; return -1; }
    assert(control->operation == RELIEFOS_NET_CONTROL_CONFIG);
    control->data.config.local_ip = 0x0a00020f;
    return 0;
}
int reliefos_openrc_run(const char *service, const char *action)
{ assert(!strcmp(service, "reliefos-dhcp") && !strcmp(action, "restart")); return 1; }
int main(void)
{
    struct reliefos_net_config config;
    assert(reliefos_net_config(&config) == 0 && config.local_ip == 0x0a00020f && operations == 1);
    failure = 1;
    assert(reliefos_net_config(&config) == -1 && errno == ENETDOWN);
    assert(reliefos_net_config(NULL) == -1 && errno == EINVAL);
    struct reliefos_net_dhcp lease;
    assert(reliefos_net_dhcp_renew(100, &lease) == -1 && errno == EIO);
    assert(lease.status == RELIEFOS_NET_STATUS_DHCP_FAILED && operations == 2);
    char path[256];
    host_tmp_path(path, sizeof(path), "lease-XXXXXX");
    int fd = mkstemp(path);
    assert(fd >= 0);
    lease_test_path = path;
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    char text[256];
    int size = snprintf(text, sizeof(text), "ip=10.0.2.15\nsubnet=255.255.255.0\ndns=10.0.2.3\nlease=60\nacquired=%lld\n", (long long)now.tv_sec);
    assert(write(fd, text, size) == size);
    assert(fchmod(fd, 0600) == 0);
    config = (struct reliefos_net_config){.local_ip=0x0a00020f, .subnet_mask=0xffffff00};
    net_merge_dhcp_lease(&config);
    if (geteuid() == 0) {
        assert(config.source == RELIEFOS_NET_CONFIG_SOURCE_DHCP && config.dns_ip == 0x0a000203);
    } else assert(config.source != RELIEFOS_NET_CONFIG_SOURCE_DHCP);
    assert(fchmod(fd, 0666) == 0);
    config.source = 0;
    net_merge_dhcp_lease(&config);
    assert(config.source == 0);
    assert(fchmod(fd, 0600) == 0);
    config.local_ip = 0x0a000210;
    net_merge_dhcp_lease(&config);
    assert(config.source == 0);
    close(fd);
    unlink(path);
    puts("PASS network adapter, lease ownership/mode/address checks and failed OpenRC renewal");
}
