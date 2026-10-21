#include <reliefnt/console.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../kernel/reliefnt/tools/tests/modules/network.h"

static uint64_t now_ms;
static uint8_t sent_frame[2048];
static uint32_t sent_length;
static uint32_t sent_count;
static int send_error;
static int allocation_failure;
static int buffers;
void *kernel_malloc(size_t size)
{
    void *p = allocation_failure ? NULL : malloc(size);
    if (p) ++buffers;
    return p;
}
void kernel_free(void *p) { if (p) { --buffers; free(p); } }
uint64_t time_uptime_ms(void) { return now_ms; }
const uint8_t *e1000_mac(void) { static const uint8_t mac[6] = {2, 3, 4, 5, 6, 7}; return mac; }
void e1000_get_info(struct e1000_info *info) { *info = (struct e1000_info){.present=1, .active=1}; }
int e1000_send(const void *data, uint32_t length)
{ assert(length <= sizeof(sent_frame)); __builtin_memcpy(sent_frame, data, length); sent_length = length; ++sent_count; return send_error; }
int e1000_poll(void *frame, uint32_t capacity, uint32_t *length)
{ (void)frame; (void)capacity; *length = 0; return 0; }
void net_udp_input(uint32_t source, uint32_t destination, uint16_t source_port,
                   uint16_t destination_port, const void *data, uint32_t length, int ifindex)
{ (void)ifindex; (void)source; (void)destination; (void)source_port; (void)destination_port; (void)data; (void)length; }
void net_packet_input(const void *frame, uint32_t length) { (void)frame; (void)length; }
void console_printf(const char *format, ...) { (void)format; }

void console_log(enum console_log_level level, const char *format, ...)
{
    (void)level; (void)format;
}

static void test_small_reads(struct net_socket *s)
{
    s->remote_seq += NET_SOCKET_RX_CAP - s->rx_len;
    s->rx_len = NET_SOCKET_RX_CAP;
    for (unsigned i = 0; i < NET_SOCKET_RX_CAP; ++i) s->rx[i] = i % 251;
    assert(net_send_tcp_to_mac(s->dst_mac, s->local_ip, s->remote_ip,
        s->local_port, s->remote_port, s->local_seq, s->remote_seq, TCP_FLAG_ACK, 0, 0) == 0);
    assert(net_get_u16(sent_frame + 48) == 0);
    uint8_t data[NET_SOCKET_RX_CAP];
    struct reliefos_net_socket_io request = {.socket=s->handle, .buffer=data, .length=5};
    unsigned before = sent_count;
    assert(net_socket_recv(&request, 0) == 0 && request.transferred == 5);
    assert(sent_count == before); /* A TLS header must not reopen a five-byte window. */
    for (unsigned i = 0; i < 5; ++i) assert(data[i] == i);
    request.length = NET_TCP_MSS - 5;
    assert(net_socket_recv(&request, 0) == 0 && request.transferred == NET_TCP_MSS - 5);
    assert(sent_count == before + 1 && net_get_u16(sent_frame + 48) >= NET_TCP_MSS);
    for (unsigned i = 0; i < request.transferred; ++i) assert(data[i] == (i + 5) % 251);
    request.length = NET_SOCKET_RX_CAP;
    assert(net_socket_recv(&request, 0) == 0 && request.transferred == NET_SOCKET_RX_CAP - NET_TCP_MSS);
    for (unsigned i = 0; i < request.transferred; ++i) assert(data[i] == (i + NET_TCP_MSS) % 251);
    /* Fill and drain across the end of the receive storage. */
    uint8_t incoming[NET_TCP_MSS];
    for (unsigned i = 0; i < sizeof(incoming); ++i) incoming[i] = i % 251;
    for (unsigned round = 0; round < 100; ++round) {
        net_socket_handle_tcp(s->remote_ip, s->remote_port, s->local_port,
            s->remote_seq, s->local_seq, TCP_FLAG_ACK, incoming, sizeof(incoming));
        request.length = 17;
        assert(net_socket_recv(&request, 0) == 0 && request.transferred == 17);
        assert(!__builtin_memcmp(data, incoming, 17));
        request.length = sizeof(data);
        assert(net_socket_recv(&request, 0) == 0 && request.transferred == sizeof(incoming) - 17);
        assert(!__builtin_memcmp(data, incoming + 17, sizeof(incoming) - 17));
    }
}

static void test_window_edges(struct net_socket *s)
{
    s->remote_seq = 0xfffffff0u;
    s->rx_window_valid = false;
    assert(!s->rx_len);
    assert(net_send_tcp_to_mac(s->dst_mac, s->local_ip, s->remote_ip,
        s->local_port, s->remote_port, s->local_seq, s->remote_seq, TCP_FLAG_ACK, 0, 0) == 0);
    uint32_t end = net_get_u32(sent_frame + 42) + net_get_u16(sent_frame + 48);
    uint8_t input[20] = {0};
    net_socket_handle_tcp(s->remote_ip, s->remote_port, s->local_port,
        s->remote_seq, s->local_seq, TCP_FLAG_ACK, input, sizeof(input));
    assert(s->remote_seq == 4);
    uint32_t next_end = net_get_u32(sent_frame + 42) + net_get_u16(sent_frame + 48);
    assert((int32_t)(next_end - end) >= 0);
    assert(net_get_u16(sent_frame + 48) <= NET_SOCKET_RX_CAP - s->rx_len);
    s->remote_seq += NET_SOCKET_RX_CAP - s->rx_len;
    s->rx_len = NET_SOCKET_RX_CAP;
    assert(net_send_tcp_to_mac(s->dst_mac, s->local_ip, s->remote_ip,
        s->local_port, s->remote_port, s->local_seq, s->remote_seq, TCP_FLAG_ACK, 0, 0) == 0);
    assert(net_get_u16(sent_frame + 48) == 0);
    uint8_t data[NET_TCP_MSS];
    struct reliefos_net_socket_io request = {.socket=s->handle, .buffer=data, .length=sizeof(data)};
    send_error = -1;
    assert(net_socket_recv(&request, 0) == 0 && request.transferred == sizeof(data));
    assert(net_socket_receive_window(s) == 0); /* Failed TX must not publish state. */
    send_error = 0;
    assert(net_socket_recv(&request, 0) == 0 && request.transferred == sizeof(data));
    assert(net_get_u16(sent_frame + 48) == 2 * sizeof(data));
}

int main(void)
{
    struct net_socket *s = net_socket_alloc(7, 1000);
    assert(s);
    /* An open descriptor must survive idle GC and table pressure. */
    net_socket_pin_fd(s->handle);
    int handle = s->handle;
    now_ms = NET_SOCKET_CLOSE_HOLD_MS + 1;
    net_socket_gc();
    assert(net_socket_find(handle, 0, 1) == s);
    s->state = RELIEFOS_NET_TCP_ESTABLISHED;
    s->local_ip = 0x0a25000f; s->remote_ip = 0x0a250002;
    s->local_port = 51000; s->remote_port = 8000;
    s->remote_seq = 500; s->local_seq = 100;
    s->rx_len = NET_SOCKET_RX_CAP - 2;
    const uint8_t bytes[] = {1, 2, 3, 4};
    net_socket_handle_tcp(s->remote_ip, s->remote_port, s->local_port,
                          500, 100, TCP_FLAG_ACK, bytes, sizeof(bytes));
    assert(s->rx_len == NET_SOCKET_RX_CAP);
    assert(s->remote_seq == 502);
    assert(sent_length >= 54 && net_get_u16(sent_frame + 48) == 0);
    s->rx_len = 0;
    net_socket_handle_tcp(s->remote_ip, s->remote_port, s->local_port,
                          500, 100, TCP_FLAG_ACK, bytes, sizeof(bytes));
    assert(s->rx_len == 2 && s->remote_seq == 504 && s->rx[0] == 3 && s->rx[1] == 4);
    test_small_reads(s);
    test_window_edges(s);
    net_close_owner_sockets(7);
    assert(s->state == RELIEFOS_NET_TCP_ESTABLISHED);
    net_socket_release_fd(handle);
    assert(!s->fd_owned);
    struct net_dhcp_offer lease = {.yiaddr=0x0a25000f, .subnet_mask=0xffffff00,
        .router_ip=0x0a250002, .dns_ip=0x0a250003, .server_ip=0x0a250002, .lease_seconds=10};
    net_apply_dhcp_offer(&lease);
    struct reliefos_net_config config;
    now_ms += 3000;
    net_get_config(&config);
    assert(config.lease_seconds == 7 && config.dns_ip == lease.dns_ip);
    now_ms += 7000;
    net_get_config(&config);
    assert(!config.local_ip && !config.dns_ip && config.source == RELIEFOS_NET_CONFIG_SOURCE_NONE);
    now_ms += NET_SOCKET_CLOSE_HOLD_MS + 1;
    net_socket_gc();
    assert(buffers == 0);
    allocation_failure = 1;
    assert(net_socket_alloc(7, 1000) == NULL && buffers == 0);
    allocation_failure = 0;
    s = net_socket_alloc(7, 1000);
    assert(s && buffers == 1);
    net_socket_clear(s);
    assert(buffers == 0);
    puts("[tcp-state] PASS descriptor lifetime and receive-window integrity");
    return 0;
}
