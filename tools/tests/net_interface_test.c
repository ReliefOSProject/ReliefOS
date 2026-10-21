#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/reliefnt/tools/tests/modules/network.h"

static struct task current;
struct task *sched_current_task(void) { return &current; }
int e1000_is_ready(void) { return true; }
const uint8_t *e1000_mac(void) { static const uint8_t mac[6] = {2,3,4,5,6,7}; return mac; }
bool user_range_ok(uint64_t address, uint64_t size) { return address >= 4096 && size <= 4096; }
bool user_range_writable(uint64_t address, uint64_t size) { return user_range_ok(address, size); }
static int call(unsigned command, struct ifreq *req) { return net_interface_ioctl(command, (uintptr_t)req); }
int main(void)
{
    struct ifreq req = {.ifr_name = "eth0"};
    assert(call(SIOCGIFINDEX, &req) == 0 && req.data.value == 2);
    assert(call(SIOCGIFHWADDR, &req) == 0 && req.data.addr.sa_family == 1 && req.data.addr.sa_data[0] == 2);
    req.data.addr.sa_family = AF_INET;
    net_put_u32((uint8_t *)&req.data.addr + 4, 0x0a00020f);
    assert(call(SIOCSIFADDR, &req) == -LINUX_EPERM);
    current.cap_effective = 1ULL << CAP_NET_ADMIN;
    assert(call(SIOCSIFADDR, &req) == 0 && net_config.local_ip == 0x0a00020f);
    net_put_u32((uint8_t *)&req.data.addr + 4, 0xff00ff00);
    assert(call(SIOCSIFNETMASK, &req) == -LINUX_EINVAL);
    net_put_u32((uint8_t *)&req.data.addr + 4, 0xffffff00);
    assert(call(SIOCSIFNETMASK, &req) == 0 && net_config.subnet_mask == 0xffffff00);
    assert(call(SIOCGIFBRDADDR, &req) == 0);
    assert(net_get_u32((uint8_t *)&req.data.addr + 4) == 0x0a0002ff);
    struct rtentry rt = {.rt_dst.sa_family = AF_INET, .rt_gateway.sa_family = AF_INET,
        .rt_genmask.sa_family = 0, .rt_flags = 3};
    net_put_u32((uint8_t *)&rt.rt_gateway + 4, 0x0a000202);
    assert(net_interface_ioctl(SIOCADDRT, (uintptr_t)&rt) == 0);
    assert(net_config.gateway_ip == 0x0a000202);
    assert(net_interface_ioctl(SIOCADDRT, (uintptr_t)&rt) == -LINUX_EEXIST);
    assert(net_interface_ioctl(SIOCDELRT, (uintptr_t)&rt) == 0);
    assert(net_interface_ioctl(SIOCDELRT, (uintptr_t)&rt) == -LINUX_ESRCH);
    struct ifreq output;
    struct ifconf conf = {.ifc_len = sizeof(output), .ifc_buf = (uintptr_t)&output};
    assert(net_interface_ioctl(SIOCGIFCONF, (uintptr_t)&conf) == 0);
    assert(conf.ifc_len == sizeof(output) && !strcmp(output.ifr_name, "eth0"));
    assert(net_get_u32((uint8_t *)&output.data.addr + 4) == 0x0a00020f);
    req.data.flags = 0;
    assert(call(SIOCSIFFLAGS, &req) == 0 && !net_interface_ready());
    req.data.flags = IFF_UP;
    assert(call(SIOCSIFFLAGS, &req) == 0 && net_interface_ready());
    strcpy(req.ifr_name, "absent");
    assert(call(SIOCGIFINDEX, &req) == -LINUX_ENODEV);
    assert(net_interface_ioctl(SIOCGIFINDEX, 0) == -LINUX_EFAULT);
    puts("PASS real kernel interface ioctl permissions, address/mask, route, enumeration, up/down");
}
