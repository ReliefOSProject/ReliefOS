#include <reliefos/fs.h>
#include <reliefos/gui.h>
#include <libintl.h>
#include <locale.h>
#include <reliefos/layout.h>
#include <reliefos/net_service.h>
#include <reliefos/psf_font.h>
#include <reliefos/stdio.h>
#include <reliefos/syscall.h>
#include <reliefos/ui.h>
#include <reliefos/layout.h>
#include <reliefos/sudo.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define NETCTL_W 720
#define NETCTL_H 584
#define DOMAIN_LEN NET_SERVICE_HOSTNAME_LEN
#define DNS_INPUT_LEN 16U
#define NETCTL_NETWORK_CONFIG_PATH RELIEFOS_PATH_NETWORK_CONF
#define NETCTL_NETWORK_CONFIG_TEMP_PATH RELIEFOS_PATH_NETWORK_TMP
#define NETCTL_NETWORK_CONFIG_BACKUP_PATH RELIEFOS_PATH_NETWORK_BAK
#define NETCTL_SAVE_RETRIES 3U
#define CONN_VISIBLE_ROWS 4U
#define CONN_ROW_H (RELIEFOS_FONT_H + 4U)
#define CONN_X 34U
#define CONN_Y 398U
#define CONN_W (NETCTL_W - 68U)
#define CONN_ROWS_Y (CONN_Y + RELIEFOS_FONT_H + 8U)
#define T(s) gettext(s)

static uint32_t pixels[NETCTL_W * NETCTL_H];
static net_service_config_t config;
static uint32_t dns_mode = NET_SERVICE_DNS_MODE_CLOUDFLARE;
static uint32_t dns_custom_ip;
static net_service_connection_info_t connections[NET_SERVICE_SOCKET_MAX];
static uint32_t connection_count;
static char domain_input[DOMAIN_LEN] = "example.com";
static char dns_input[DNS_INPUT_LEN] = "1.1.1.1";
static char status_text[128] = "Ready";
static char dns_text[192] = "Enter a host name and resolve an A record.";
static struct reliefos_ui_edit_state domain_edit;
static struct reliefos_ui_edit_state dns_edit;
static struct reliefos_ui_listview_state connections_view;
static uint32_t dhcp_child;

static void copy_text(char *dst, uint32_t cap, const char *src)
{
    uint32_t i = 0;
    if (!dst || cap == 0) {
        return;
    }
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = 0;
}

static void append_char(char *dst, uint32_t *pos, uint32_t cap, char ch)
{
    if (dst && pos && *pos + 1 < cap) {
        dst[*pos] = ch;
        ++(*pos);
        dst[*pos] = 0;
    }
}

static void append_text(char *dst, uint32_t *pos, uint32_t cap, const char *text)
{
    while (text && *text) {
        append_char(dst, pos, cap, *text++);
    }
}

static void append_u32(char *dst, uint32_t *pos, uint32_t cap, uint32_t value)
{
    char tmp[12];
    uint32_t n = 0;
    if (value == 0) {
        append_char(dst, pos, cap, '0');
        return;
    }
    while (value && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (n) {
        append_char(dst, pos, cap, tmp[--n]);
    }
}

static void append_i32(char *dst, uint32_t *pos, uint32_t cap, int value)
{
    if (value < 0) {
        append_char(dst, pos, cap, '-');
        value = -value;
    }
    append_u32(dst, pos, cap, (uint32_t)value);
}

static void append_hex_nibble(char *dst, uint32_t *pos, uint32_t cap, uint8_t value)
{
    value &= 0x0fu;
    append_char(dst, pos, cap, value < 10 ? (char)('0' + value)
                                          : (char)('a' + value - 10));
}

static void append_hex_byte(char *dst, uint32_t *pos, uint32_t cap, uint8_t value)
{
    append_hex_nibble(dst, pos, cap, (uint8_t)(value >> 4));
    append_hex_nibble(dst, pos, cap, value);
}

static void format_ipv4(char *dst, uint32_t cap, uint32_t ip)
{
    uint32_t pos = 0;
    dst[0] = 0;
    append_u32(dst, &pos, cap, (ip >> 24) & 0xffu);
    append_char(dst, &pos, cap, '.');
    append_u32(dst, &pos, cap, (ip >> 16) & 0xffu);
    append_char(dst, &pos, cap, '.');
    append_u32(dst, &pos, cap, (ip >> 8) & 0xffu);
    append_char(dst, &pos, cap, '.');
    append_u32(dst, &pos, cap, ip & 0xffu);
}

static int parse_ipv4(const char *text, uint32_t *out_ip)
{
    uint32_t ip = 0;
    uint32_t pos = 0;
    if (!text || !out_ip) {
        return 0;
    }
    for (uint32_t octet = 0; octet < 4U; ++octet) {
        uint32_t value = 0;
        uint32_t digits = 0;
        while (text[pos] >= '0' && text[pos] <= '9') {
            value = value * 10U + (uint32_t)(text[pos] - '0');
            if (value > 255U || ++digits > 3U) {
                return 0;
            }
            ++pos;
        }
        if (!digits) {
            return 0;
        }
        ip = (ip << 8) | value;
        if (octet != 3U) {
            if (text[pos++] != '.') {
                return 0;
            }
        }
    }
    if (text[pos] || ip == 0 || ip == 0xffffffffU) {
        return 0;
    }
    *out_ip = ip;
    return 1;
}

static const char *dns_mode_name(uint32_t mode)
{
    switch (mode) {
    case NET_SERVICE_DNS_MODE_DHCP:
        return T("DHCP DNS");
    case NET_SERVICE_DNS_MODE_CUSTOM:
        return T("Custom DNS");
    default:
        return T("Cloudflare DNS");
    }
}

static void load_dns_input(uint32_t ip)
{
    if (!ip) {
        ip = NET_SERVICE_CLOUDFLARE_DNS_IP;
    }
    format_ipv4(dns_input, sizeof(dns_input), ip);
}

static void format_endpoint(char *dst, uint32_t cap, uint32_t ip, uint32_t port)
{
    uint32_t pos = 0;
    dst[0] = 0;
    append_u32(dst, &pos, cap, (ip >> 24) & 0xffu);
    append_char(dst, &pos, cap, '.');
    append_u32(dst, &pos, cap, (ip >> 16) & 0xffu);
    append_char(dst, &pos, cap, '.');
    append_u32(dst, &pos, cap, (ip >> 8) & 0xffu);
    append_char(dst, &pos, cap, '.');
    append_u32(dst, &pos, cap, ip & 0xffu);
    append_char(dst, &pos, cap, ':');
    append_u32(dst, &pos, cap, port);
}

static void format_mac(char *dst, uint32_t cap, const uint8_t mac[6])
{
    uint32_t pos = 0;
    dst[0] = 0;
    for (uint32_t i = 0; i < 6; ++i) {
        if (i) {
            append_char(dst, &pos, cap, ':');
        }
        append_hex_byte(dst, &pos, cap, mac[i]);
    }
}

static const char *status_name(uint32_t status)
{
    switch (status) {
    case NET_SERVICE_STATUS_OK:
        return T("Succeeded");
    case NET_SERVICE_STATUS_NO_DEVICE:
        return T("No e1000 adapter");
    case NET_SERVICE_STATUS_ARP_TIMEOUT:
        return T("ARP timeout");
    case NET_SERVICE_STATUS_BAD_ARGUMENT:
        return T("Bad argument");
    case NET_SERVICE_STATUS_TX_FAILED:
        return T("Transmit failed");
    case NET_SERVICE_STATUS_DHCP_TIMEOUT:
        return T("DHCP timeout");
    case NET_SERVICE_STATUS_DHCP_FAILED:
        return T("DHCP failed");
    case NET_SERVICE_STATUS_DNS_TIMEOUT:
        return T("DNS timeout");
    case NET_SERVICE_STATUS_DNS_FAILED:
        return T("DNS failed");
    case NET_SERVICE_STATUS_DNS_NO_ANSWER:
        return T("No A record");
    case NET_SERVICE_STATUS_TCP_TIMEOUT:
        return T("TCP timeout");
    case NET_SERVICE_STATUS_TCP_RESET:
        return T("TCP reset");
    case NET_SERVICE_STATUS_TCP_FAILED:
        return T("TCP failed");
    case NET_SERVICE_STATUS_HTTP_FAILED:
        return T("HTTP failed");
    case NET_SERVICE_STATUS_HTTP_TOO_LARGE:
        return T("Response too large");
    case NET_SERVICE_STATUS_SOCKET_LIMIT:
        return T("Socket limit reached");
    case NET_SERVICE_STATUS_SOCKET_BAD_HANDLE:
        return T("Bad socket");
    case NET_SERVICE_STATUS_SOCKET_NOT_CONNECTED:
        return T("Socket not connected");
    case NET_SERVICE_STATUS_SOCKET_CLOSED:
        return T("Socket closed");
    case NET_SERVICE_STATUS_PROTOCOL_UNSUPPORTED:
        return T("Protocol unsupported");
    default:
        return T("Unknown status");
    }
}

static const char *connection_state_name(uint32_t state)
{
    switch (state) {
    case NET_SERVICE_TCP_SYN_SENT:
        return "SYN_SENT";
    case NET_SERVICE_TCP_ESTABLISHED:
        return "ESTABLISHED";
    case NET_SERVICE_TCP_TIME_WAIT:
        return "TIME_WAIT";
    case NET_SERVICE_TCP_CLOSED:
        return "CLOSED";
    default:
        return "UNKNOWN";
    }
}

static const char *source_name(uint32_t source)
{
    switch (source) {
    case NET_SERVICE_CONFIG_SOURCE_DHCP:
        return "DHCP";
    case NET_SERVICE_CONFIG_SOURCE_STATIC:
        return T("Static fallback");
    default:
        return T("None");
    }
}

static void set_status_ret(const char *prefix, int ret)
{
    int error = errno;
    uint32_t pos = 0;
    status_text[0] = 0;
    append_text(status_text, &pos, sizeof(status_text), prefix);
    append_text(status_text, &pos, sizeof(status_text), " ret=");
    append_i32(status_text, &pos, sizeof(status_text), ret);
    if (ret < 0 && error) {
        append_text(status_text, &pos, sizeof(status_text), ": ");
        append_text(status_text, &pos, sizeof(status_text), strerror(error));
    }
}

static void refresh_connections(void);

static int refresh_dns_policy(void)
{
    net_service_dns_policy_t policy;
    int ret = net_service_get_dns_policy(&policy);
    if (ret < 0) {
        set_status_ret(T("DNS policy query failed"), ret);
        return ret;
    }
    if (policy.status != NET_SERVICE_STATUS_OK) {
        copy_text(status_text, sizeof(status_text), status_name(policy.status));
        return -1;
    }
    dns_mode = policy.mode;
    dns_custom_ip = policy.custom_dns_ip;
    return 0;
}

static void refresh_config(void)
{
    int ret = net_service_config(&config);
    if (ret < 0) {
        config = (net_service_config_t){0};
        set_status_ret(T("Config query failed"), ret);
        return;
    }
    if (refresh_dns_policy() < 0) {
        return;
    }
    copy_text(status_text, sizeof(status_text), T("Network configuration refreshed"));
    refresh_connections();
}

static void refresh_connections(void)
{
    int ret = net_service_connections(connections, NET_SERVICE_SOCKET_MAX,
                                     &connection_count);
    if (ret < 0) {
        connection_count = 0;
        set_status_ret(T("Connection query failed"), ret);
        return;
    }
    reliefos_ui_listview_state_set_count(&connections_view, connection_count);
}

static void renew_dhcp(void)
{
    if (dhcp_child) return;
    if (geteuid() != 0) {
        char *args[] = {"/usr/lib/reliefos/apps/netctl/netctl.elf", "--renew-dhcp", NULL};
        if (reliefos_sudo_run(NULL, NULL, args, &dhcp_child) < 0) {
            set_status_ret(T("Authorization failed"), -1);
            return;
        }
        copy_text(status_text, sizeof(status_text), T("Authorizing DHCP update..."));
        return;
    }
    net_service_dhcp_t dhcp;
    int ret = net_service_dhcp_renew(4000, &dhcp);
    if (ret < 0) {
        set_status_ret(T("DHCP request failed"), ret);
        return;
    }
    config = dhcp.config;
    if (refresh_dns_policy() < 0) {
        return;
    }
    refresh_connections();
    copy_text(status_text, sizeof(status_text), status_name(dhcp.status));
}

/* A separate sudo-authorized process performs only this operation. The GUI
 * stays unprivileged; sudoers and PAM decide whether the helper may run. */
static int renew_dhcp_command(void)
{
    if (geteuid() != 0) return 128 + EACCES;
    net_service_dhcp_t result;
    if (net_service_dhcp_renew(4000, &result) < 0) {
        int error = errno;
        fprintf(stderr, "[netctl] DHCP request failed: %s (errno=%d)\n", strerror(error), error);
        return 128 + (error > 0 && error < 128 ? error : EIO);
    }
    if (result.status != NET_SERVICE_STATUS_OK)
        return result.status < 64 ? 64 + (int)result.status : 128 + EIO;
    return 0;
}

static int poll_dhcp_command(void)
{
    if (!dhcp_child) return 0;
    int status;
    if (reliefos_sudo_wait(dhcp_child, &status) < 0) {
        if (errno == EAGAIN || errno == EINTR) return 0;
        dhcp_child = 0;
        set_status_ret(T("DHCP worker failed"), -1);
        return 1;
    }
    dhcp_child = 0;
    if (!WIFEXITED(status)) {
        copy_text(status_text, sizeof(status_text), T("DHCP update interrupted"));
        return 1;
    }
    int code = WEXITSTATUS(status);
    if (code == 0) refresh_config();
    else if (code >= 128) {
        errno = code - 128;
        set_status_ret(T("DHCP request failed"), -1);
    } else if (code >= 64) {
        copy_text(status_text, sizeof(status_text), status_name((uint32_t)code - 64));
    } else {
        copy_text(status_text, sizeof(status_text),
                  T("Authorization denied or cancelled"));
    }
    return 1;
}

static int path_exists(const char *path)
{
    int fd = open(path, RELIEFOS_O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    close(fd);
    return 1;
}

static int save_dns_policy(uint32_t mode, uint32_t custom_dns_ip)
{
    char config_text[96];
    const char *mode_text;
    uint32_t pos = 0;
    int fd;
    long wrote;
    if (mode == NET_SERVICE_DNS_MODE_DHCP) {
        mode_text = "dhcp";
    } else if (mode == NET_SERVICE_DNS_MODE_CUSTOM) {
        mode_text = "custom";
    } else {
        mode_text = "cloudflare";
    }
    append_text(config_text, &pos, sizeof(config_text), "dns_mode=");
    append_text(config_text, &pos, sizeof(config_text), mode_text);
    append_char(config_text, &pos, sizeof(config_text), '\n');
    if (mode == NET_SERVICE_DNS_MODE_CUSTOM) {
        char ip[DNS_INPUT_LEN];
        format_ipv4(ip, sizeof(ip), custom_dns_ip);
        append_text(config_text, &pos, sizeof(config_text), "dns_custom=");
        append_text(config_text, &pos, sizeof(config_text), ip);
        append_char(config_text, &pos, sizeof(config_text), '\n');
    }
    for (uint32_t attempt = 0; attempt < NETCTL_SAVE_RETRIES; ++attempt) {
        unlink(NETCTL_NETWORK_CONFIG_TEMP_PATH);
        if (!path_exists(NETCTL_NETWORK_CONFIG_PATH) &&
            path_exists(NETCTL_NETWORK_CONFIG_BACKUP_PATH) &&
            rename(NETCTL_NETWORK_CONFIG_BACKUP_PATH,
                   NETCTL_NETWORK_CONFIG_PATH) < 0) {
            return -1;
        }
        if (path_exists(NETCTL_NETWORK_CONFIG_PATH)) {
            unlink(NETCTL_NETWORK_CONFIG_BACKUP_PATH);
        }
        fd = open(NETCTL_NETWORK_CONFIG_TEMP_PATH,
                  RELIEFOS_O_WRONLY | RELIEFOS_O_CREAT | RELIEFOS_O_TRUNC, 0666);
        if (fd >= 0) {
            wrote = write(fd, config_text, pos);
            if (close(fd) == 0 && wrote == (long)pos) {
                if (!path_exists(NETCTL_NETWORK_CONFIG_PATH)) {
                    if (rename(NETCTL_NETWORK_CONFIG_TEMP_PATH,
                               NETCTL_NETWORK_CONFIG_PATH) == 0) {
                        return 0;
                    }
                } else if (rename(NETCTL_NETWORK_CONFIG_PATH,
                                  NETCTL_NETWORK_CONFIG_BACKUP_PATH) == 0) {
                    if (rename(NETCTL_NETWORK_CONFIG_TEMP_PATH,
                               NETCTL_NETWORK_CONFIG_PATH) == 0) {
                        unlink(NETCTL_NETWORK_CONFIG_BACKUP_PATH);
                        return 0;
                    }
                    if (rename(NETCTL_NETWORK_CONFIG_BACKUP_PATH,
                               NETCTL_NETWORK_CONFIG_PATH) < 0) {
                        return -1;
                    }
                }
            }
        }
        unlink(NETCTL_NETWORK_CONFIG_TEMP_PATH);
        sleep_ms(20);
    }
    return -1;
}

static void apply_dns_policy(uint32_t mode)
{
    net_service_dns_policy_t policy;
    net_service_dns_policy_t previous;
    uint32_t custom_dns_ip = 0;
    int ret;
    if (mode == NET_SERVICE_DNS_MODE_CUSTOM &&
        !parse_ipv4(dns_input, &custom_dns_ip)) {
        copy_text(status_text, sizeof(status_text),
                  T("Enter a valid custom IPv4 DNS address."));
        return;
    }
    if (net_service_get_dns_policy(&previous) < 0 ||
        previous.status != NET_SERVICE_STATUS_OK) {
        copy_text(status_text, sizeof(status_text),
                  T("Unable to read the current DNS policy."));
        return;
    }
    ret = net_service_set_dns_policy(mode, custom_dns_ip, &policy);
    if (ret < 0) {
        set_status_ret(T("DNS policy change failed"), ret);
        return;
    }
    if (policy.status != NET_SERVICE_STATUS_OK) {
        copy_text(status_text, sizeof(status_text), status_name(policy.status));
        return;
    }
    config = policy.config;
    dns_mode = policy.mode;
    dns_custom_ip = policy.custom_dns_ip;
    refresh_connections();
    if (save_dns_policy(mode, custom_dns_ip) < 0) {
        net_service_dns_policy_t restored;
        int restore_ret = net_service_set_dns_policy(previous.mode,
                                                     previous.custom_dns_ip,
                                                     &restored);
        if (restore_ret == 0 && restored.status == NET_SERVICE_STATUS_OK) {
            config = restored.config;
            dns_mode = restored.mode;
            dns_custom_ip = restored.custom_dns_ip;
            copy_text(status_text, sizeof(status_text),
                      T("DNS save failed; previous policy restored."));
        } else {
            copy_text(status_text, sizeof(status_text),
                      T("DNS save failed; runtime policy may differ."));
        }
        return;
    }
    load_dns_input(mode == NET_SERVICE_DNS_MODE_CUSTOM
                       ? custom_dns_ip
                       : NET_SERVICE_CLOUDFLARE_DNS_IP);
    copy_text(status_text, sizeof(status_text),
              T("DNS policy applied and saved"));
}

static void resolve_domain(void)
{
    net_service_dns_t dns;
    int ret = net_service_dns_resolve(domain_input, 4000, &dns);
    uint32_t pos = 0;
    if (ret < 0) {
        set_status_ret(T("DNS ioctl failed"), ret);
        return;
    }
    dns_text[0] = 0;
    if (dns.status == NET_SERVICE_STATUS_OK && dns.address_count) {
        append_text(dns_text, &pos, sizeof(dns_text), domain_input);
        append_text(dns_text, &pos, sizeof(dns_text), " -> ");
        for (uint32_t i = 0; i < dns.address_count; ++i) {
            char ip[24];
            if (i) {
                append_text(dns_text, &pos, sizeof(dns_text), ", ");
            }
            format_ipv4(ip, sizeof(ip), dns.addresses[i]);
            append_text(dns_text, &pos, sizeof(dns_text), ip);
        }
    } else {
        append_text(dns_text, &pos, sizeof(dns_text), status_name(dns.status));
        append_text(dns_text, &pos, sizeof(dns_text), T(" resolving "));
        append_text(dns_text, &pos, sizeof(dns_text), domain_input);
    }
    copy_text(status_text, sizeof(status_text), status_name(dns.status));
    refresh_connections();
}

static void draw_row(struct reliefos_ui_surface *ui, uint32_t y,
                     const char *label, const char *value)
{
    reliefos_ui_text(ui, 26, y, label, RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
    reliefos_ui_text_clipped(ui, 146, y, NETCTL_W - 170, value, RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
}

static void draw_netctl(struct reliefos_ui_surface *ui)
{
    static const struct reliefos_ui_list_column conn_cols[] = {
        { "Socket", 58 },
        { "PID", 48 },
        { "State", 112 },
        { "Local", 138 },
        { "Remote", 188 },
        { "Bytes", 104 },
    };
    char mac[32];
    char ip[32];
    char mask[32];
    char gateway[32];
    char dns[32];
    char lease[48];
    const char *dns_mode_text;
    uint32_t pos = 0;

    format_mac(mac, sizeof(mac), config.mac);
    format_ipv4(ip, sizeof(ip), config.local_ip);
    format_ipv4(mask, sizeof(mask), config.subnet_mask);
    format_ipv4(gateway, sizeof(gateway), config.gateway_ip);
    format_ipv4(dns, sizeof(dns), config.dns_ip);
    dns_mode_text = dns_mode_name(dns_mode);
    lease[0] = 0;
    append_u32(lease, &pos, sizeof(lease), config.lease_seconds);
    append_text(lease, &pos, sizeof(lease), "s");

    reliefos_ui_rect(ui, 0, 0, NETCTL_W, NETCTL_H, RELIEFOS_UI_GRAY);
    reliefos_ui_text(ui, 24, 16, T("Intel e1000 Network"), RELIEFOS_UI_BLACK, RELIEFOS_UI_GRAY);

    draw_row(ui, 50, T("State:"),
             ((config.flags & NET_SERVICE_CONFIG_FLAG_ACTIVE) &&
              (config.flags & NET_SERVICE_CONFIG_FLAG_DHCP) &&
              config.source == NET_SERVICE_CONFIG_SOURCE_DHCP)
                 ? T("Active")
                 : ((config.flags & NET_SERVICE_CONFIG_FLAG_ACTIVE)
                        ? T("No DHCP lease")
                        : T("Unavailable")));
    draw_row(ui, 74, T("MAC:"), mac);
    draw_row(ui, 98, T("Config:"), source_name(config.source));
    draw_row(ui, 122, T("IPv4:"), ip);
    draw_row(ui, 146, T("Mask:"), mask);
    draw_row(ui, 170, T("Gateway:"), gateway);
    draw_row(ui, 194, T("DNS:"), dns);
    draw_row(ui, 218, T("Lease:"), lease);
    draw_row(ui, 242, T("DNS mode:"), dns_mode_text);

    reliefos_ui_button(ui, 24, 270, 110, RELIEFOS_UI_BUTTON_H,
                     T("Cloudflare"),
                     dns_mode == NET_SERVICE_DNS_MODE_CLOUDFLARE
                         ? RELIEFOS_UI_BUTTON_PRESSED : 0);
    reliefos_ui_button(ui, 144, 270, 96, RELIEFOS_UI_BUTTON_H, T("DHCP"),
                     dns_mode == NET_SERVICE_DNS_MODE_DHCP
                         ? RELIEFOS_UI_BUTTON_PRESSED : 0);
    reliefos_ui_button(ui, 250, 270, 92, RELIEFOS_UI_BUTTON_H, T("Custom"),
                     dns_mode == NET_SERVICE_DNS_MODE_CUSTOM
                         ? RELIEFOS_UI_BUTTON_PRESSED : 0);
    reliefos_ui_edit_state_draw(ui, 352, 270, 154, &dns_edit, 0);
    reliefos_ui_button(ui, 516, 270, 104, RELIEFOS_UI_BUTTON_H, T("Apply"), 0);

    reliefos_ui_button(ui, 24, 312, 88, RELIEFOS_UI_BUTTON_H, T("Refresh"), 0);
    reliefos_ui_button(ui, 124, 312, 118, RELIEFOS_UI_BUTTON_H, T("Renew DHCP"), 0);
    reliefos_ui_text(ui, 254, 316, T("Domain:"), RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
    reliefos_ui_edit_state_draw(ui, 298, 312, 220, &domain_edit, 0);
    reliefos_ui_button(ui, 532, 312, 86, RELIEFOS_UI_BUTTON_H, T("Resolve"), 0);
    reliefos_ui_text_clipped(ui, 24, 348, NETCTL_W - 48, dns_text,
                           RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);

    reliefos_ui_groupbox(ui, 24, 376, NETCTL_W - 48, 154,
                       T("TCP Connections"));
    reliefos_ui_listview_header(ui, CONN_X, CONN_Y, CONN_W,
                              conn_cols, 6);
    if (connection_count == 0) {
        reliefos_ui_text(ui, CONN_X + 8, CONN_ROWS_Y + 8,
                       T("No TCP client sockets."),
                       RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
    } else {
        for (uint32_t row = 0; row < CONN_VISIBLE_ROWS; ++row) {
            uint32_t index = connections_view.scroll + row;
            char socket_text[16];
            char pid_text[16];
            char local_text[40];
            char remote_text[48];
            char bytes_text[48];
            const char *cells[6];
            uint32_t bpos = 0;
            if (index >= connection_count || index >= NET_SERVICE_SOCKET_MAX) {
                break;
            }
            socket_text[0] = 0;
            pid_text[0] = 0;
            bytes_text[0] = 0;
            append_u32(socket_text, &bpos, sizeof(socket_text),
                       (uint32_t)connections[index].socket);
            bpos = 0;
            append_u32(pid_text, &bpos, sizeof(pid_text),
                       connections[index].owner_pid);
            format_endpoint(local_text, sizeof(local_text),
                            connections[index].local_ip,
                            connections[index].local_port);
            format_endpoint(remote_text, sizeof(remote_text),
                            connections[index].remote_ip,
                            connections[index].remote_port);
            bpos = 0;
            append_u32(bytes_text, &bpos, sizeof(bytes_text),
                       connections[index].tx_bytes);
            append_char(bytes_text, &bpos, sizeof(bytes_text), '/');
            append_u32(bytes_text, &bpos, sizeof(bytes_text),
                       connections[index].rx_bytes);
            cells[0] = socket_text;
            cells[1] = pid_text;
            cells[2] = connection_state_name(connections[index].state);
            cells[3] = local_text;
            cells[4] = remote_text;
            cells[5] = bytes_text;
            reliefos_ui_listview_row(ui, CONN_X, CONN_ROWS_Y + row * CONN_ROW_H,
                                   CONN_W, conn_cols, cells, 6,
                                   connections_view.selected == (int32_t)index
                                       ? RELIEFOS_UI_MENU_SELECTED
                                       : 0);
        }
    }
    reliefos_ui_statusbar(ui, NETCTL_H - 28, 28, status_text);
}

static int hit_rect(int32_t px, int32_t py, int32_t x, int32_t y,
                    uint32_t w, uint32_t h)
{
    return px >= x && py >= y &&
           px < x + (int32_t)w && py < y + (int32_t)h;
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    bindtextdomain("netctl", RELIEFOS_LAYOUT_LOCALE);
    textdomain("netctl");
    struct reliefos_ui_surface ui;
    struct reliefos_gui_app_event event;
    int window_id;

    if (argc == 2 && !strcmp(argv[1], "--renew-dhcp")) return renew_dhcp_command();
    if (argc != 1) return 2;

    puts("[netctl.elf] network controller starting");
    window_id = reliefos_gui_create_app_window_ex(T("Network Controller"),
                                                T("DHCP and DNS"),
                                                NETCTL_W, NETCTL_H,
                                                RELIEFOS_GUI_WINDOW_NO_RESIZE);
    if (window_id <= 0) {
        printf("[netctl.elf] create window failed=%d\n", window_id);
        return 1;
    }
    reliefos_ui_bind(&ui, pixels, NETCTL_W, NETCTL_H, NETCTL_W);
    reliefos_ui_edit_state_init(&domain_edit, domain_input, sizeof(domain_input));
    reliefos_ui_edit_state_init(&dns_edit, dns_input, sizeof(dns_input));
    reliefos_ui_listview_state_init(&connections_view, CONN_VISIBLE_ROWS, CONN_ROW_H);
    domain_edit.focused = 0;
    dns_edit.focused = 0;
    refresh_config();
    load_dns_input(dns_mode == NET_SERVICE_DNS_MODE_CUSTOM
                       ? dns_custom_ip
                       : NET_SERVICE_CLOUDFLARE_DNS_IP);
    draw_netctl(&ui);
    reliefos_gui_present_window((uint32_t)window_id, NETCTL_W, NETCTL_H, NETCTL_W, pixels);

    for (;;) {
        event.window_id = (uint32_t)window_id;
        while (reliefos_gui_wait_app_event(&event, RELIEFOS_GUI_IDLE_WAIT_MS) > 0) {
            if (event.type == RELIEFOS_GUI_APP_EVENT_CLOSE) {
                return 0;
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_MOUSE_BUTTON) {
                if (reliefos_ui_edit_state_handle_mouse(&dns_edit, event.x, event.y,
                                                      352, 270, 154, event.buttons) ||
                    reliefos_ui_edit_state_handle_mouse(&domain_edit, event.x, event.y,
                                                      298, 312, 220, event.buttons)) {
                    draw_netctl(&ui);
                    reliefos_gui_present_window((uint32_t)window_id, NETCTL_W, NETCTL_H, NETCTL_W, pixels);
                }
                if (reliefos_ui_listview_state_handle_mouse(&connections_view,
                                                          event.x, event.y,
                                                          CONN_X, CONN_ROWS_Y,
                                                          CONN_W, 0)) {
                    draw_netctl(&ui);
                    reliefos_gui_present_window((uint32_t)window_id, NETCTL_W, NETCTL_H, NETCTL_W, pixels);
                }
                if (event.buttons & 1u) {
                    if (hit_rect(event.x, event.y, 24, 270, 110, RELIEFOS_UI_BUTTON_H)) {
                        apply_dns_policy(NET_SERVICE_DNS_MODE_CLOUDFLARE);
                    } else if (hit_rect(event.x, event.y, 144, 270, 96, RELIEFOS_UI_BUTTON_H)) {
                        apply_dns_policy(NET_SERVICE_DNS_MODE_DHCP);
                    } else if (hit_rect(event.x, event.y, 250, 270, 92, RELIEFOS_UI_BUTTON_H)) {
                        dns_edit.focused = 1;
                        domain_edit.focused = 0;
                    } else if (hit_rect(event.x, event.y, 516, 270, 104, RELIEFOS_UI_BUTTON_H)) {
                        apply_dns_policy(NET_SERVICE_DNS_MODE_CUSTOM);
                    } else if (hit_rect(event.x, event.y, 24, 312, 88, RELIEFOS_UI_BUTTON_H)) {
                        refresh_config();
                    } else if (hit_rect(event.x, event.y, 124, 312, 118, RELIEFOS_UI_BUTTON_H)) {
                        renew_dhcp();
                    } else if (hit_rect(event.x, event.y, 532, 312, 86, RELIEFOS_UI_BUTTON_H)) {
                        resolve_domain();
                    }
                    draw_netctl(&ui);
                    reliefos_gui_present_window((uint32_t)window_id, NETCTL_W, NETCTL_H, NETCTL_W, pixels);
                }
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_MOUSE_WHEEL) {
                if (hit_rect(event.x, event.y, CONN_X, CONN_ROWS_Y,
                             CONN_W, CONN_VISIBLE_ROWS * CONN_ROW_H) &&
                    reliefos_ui_listview_state_handle_wheel(&connections_view, event.dy)) {
                    draw_netctl(&ui);
                    reliefos_gui_present_window((uint32_t)window_id, NETCTL_W, NETCTL_H, NETCTL_W, pixels);
                }
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_KEY_DOWN ||
                event.type == RELIEFOS_GUI_APP_EVENT_KEY_UP) {
                if (event.pressed && event.keycode == 1) {
                    return 0;
                }
                if (event.pressed && event.keycode == RELIEFOS_KEY_ENTER) {
                    if (dns_edit.focused) {
                        apply_dns_policy(NET_SERVICE_DNS_MODE_CUSTOM);
                    } else {
                        resolve_domain();
                    }
                } else if (!reliefos_ui_edit_state_handle_key(&dns_edit, event.keycode, event.pressed) &&
                           !reliefos_ui_edit_state_handle_key(&domain_edit, event.keycode, event.pressed)) {
                    continue;
                }
                draw_netctl(&ui);
                reliefos_gui_present_window((uint32_t)window_id, NETCTL_W, NETCTL_H, NETCTL_W, pixels);
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_FOCUS ||
                event.type == RELIEFOS_GUI_APP_EVENT_RESIZE) {
                draw_netctl(&ui);
                reliefos_gui_present_window((uint32_t)window_id, NETCTL_W, NETCTL_H, NETCTL_W, pixels);
            }
        }
        if (poll_dhcp_command()) {
            draw_netctl(&ui);
            reliefos_gui_present_window((uint32_t)window_id, NETCTL_W, NETCTL_H, NETCTL_W, pixels);
        }
        sleep_ms(10);
    }
}
