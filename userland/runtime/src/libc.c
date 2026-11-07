#include <reliefos/device.h>
#include <reliefos/environment.h>
#include <reliefos/driver.h>
#include <reliefos/auth.h>
#include <reliefos/auth.h>
#include <reliefos/audio.h>
#include <reliefos/fs.h>
#include <reliefos/system.h>
#include <reliefos/http.h>
#include <reliefos/inputm.h>
#include <reliefos/net.h>
#include <reliefos/pty.h>
#include <reliefos/stdio.h>
#include <reliefos/system.h>
#include <reliefos/syscall.h>
#include <reliefos/system.h>
#include <reliefos/text.h>
#include <reliefos/tls.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <pty.h>
#include <linux/tty.h>
#include <unistd.h>
#include <reliefos/layout.h>




int sleep_ms(unsigned long ms)
{
    struct timespec request = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000};
    return (int)syscall2(SYS_nanosleep, (long)&request, 0);
}

int reliefos_stat_legacy(const char *path, struct reliefos_stat *st)
{
    struct stat status;
    if (!st) return -EFAULT;
    if (stat(path, &status) < 0) return -errno;
    st->type = S_ISDIR(status.st_mode) ? RELIEFOS_FS_TYPE_DIR :
        (S_ISREG(status.st_mode) ? RELIEFOS_FS_TYPE_FILE : RELIEFOS_FS_TYPE_DEVICE);
    st->reserved = 0;
    st->size = status.st_size;
    return 0;
}

int reliefos_fstat_legacy(int fd, struct reliefos_stat *st)
{
    struct stat status;
    if (!st) return -EFAULT;
    if (fstat(fd, &status) < 0) return -errno;
    st->type = S_ISDIR(status.st_mode) ? RELIEFOS_FS_TYPE_DIR :
        (S_ISREG(status.st_mode) ? RELIEFOS_FS_TYPE_FILE : RELIEFOS_FS_TYPE_DEVICE);
    st->reserved = 0;
    st->size = status.st_size;
    return 0;
}




static uint32_t mode_to_legacy_permissions(uint32_t mode)
{
    return ((mode & 4u) >> 2) | (mode & 2u) | ((mode & 1u) << 2);
}

int reliefos_fs_acl_get(const char *path, struct reliefos_fs_acl *acl)
{
    struct stat st;
    if (!path || !acl) { errno = EINVAL; return -1; }
    if (stat(path, &st) < 0) return -1;
    memset(acl, 0, sizeof(*acl));
    acl->version = RELIEFOS_FS_ACL_VERSION;
    acl->owner_uid = st.st_uid;
    acl->ace_count = 3;
    const uint32_t principals[] = {RELIEFOS_FS_ACL_PRINCIPAL_OWNER,
        RELIEFOS_FS_ACL_PRINCIPAL_GROUP, RELIEFOS_FS_ACL_PRINCIPAL_EVERYONE};
    for (uint32_t i = 0; i < 3; ++i) {
        acl->aces[i].principal = principals[i];
        acl->aces[i].permissions = mode_to_legacy_permissions((st.st_mode >> (6 - 3 * i)) & 7u);
    }
    return 0;
}

int reliefos_fs_acl_set(const char *path, const struct reliefos_fs_acl *acl)
{
    struct stat st;
    if (!path || !acl || acl->version != RELIEFOS_FS_ACL_VERSION ||
        acl->ace_count > RELIEFOS_FS_ACL_MAX_ACE) { errno = EINVAL; return -1; }
    if (stat(path, &st) < 0) return -1;
    mode_t mode = st.st_mode & 07000;
    for (uint32_t i = 0; i < acl->ace_count; ++i) {
        const struct reliefos_fs_acl_ace *ace = &acl->aces[i];
        unsigned shift;
        if (ace->principal == RELIEFOS_FS_ACL_PRINCIPAL_OWNER) shift = 6;
        else if (ace->principal == RELIEFOS_FS_ACL_PRINCIPAL_GROUP) shift = 3;
        else if (ace->principal == RELIEFOS_FS_ACL_PRINCIPAL_EVERYONE) shift = 0;
        else { if (ace->permissions) { errno = ENOTSUP; return -1; } continue; }
        if (ace->flags || (ace->permissions & ~7u)) { errno = ENOTSUP; return -1; }
        uint32_t bits = ((ace->permissions & RELIEFOS_FS_PERM_READ) << 2) |
                        (ace->permissions & RELIEFOS_FS_PERM_WRITE) |
                        ((ace->permissions & RELIEFOS_FS_PERM_EXEC) >> 2);
        mode |= bits << shift;
    }
    if (st.st_uid != acl->owner_uid && chown(path, acl->owner_uid, (gid_t)-1) < 0) return -1;
    return chmod(path, mode);
}

int reliefos_fs_acl_take_ownership(const char *path, struct reliefos_fs_acl *acl)
{
    if (chown(path, getuid(), getgid()) < 0) return -1;
    return reliefos_fs_acl_get(path, acl);
}

int reliefos_fs_acl_repair(const char *path, struct reliefos_fs_acl *acl)
{
    if (chmod(path, 0700) < 0) return -1;
    return reliefos_fs_acl_get(path, acl);
}

int reliefos_text_layout_utf8(const char *text, uint32_t byte_len,
                            struct reliefos_text_glyph *glyphs,
                            uint32_t capacity,
                            struct reliefos_text_layout *out_layout)
{
    uint32_t pos = 0;
    uint32_t count = 0;
    uint32_t cells = 0;
    uint32_t pixels = 0;
    if (!text) return -1;
    if (!byte_len) {
        while (text[byte_len]) ++byte_len;
    }
    while (pos < byte_len) {
        uint8_t first = (uint8_t)text[pos];
        uint32_t sequence = 1;
        uint32_t codepoint = first;
        if ((first & 0x80u) == 0) {
            codepoint = first;
        } else if ((first & 0xe0u) == 0xc0u) {
            sequence = 2;
            codepoint = first & 0x1fu;
        } else if ((first & 0xf0u) == 0xe0u) {
            sequence = 3;
            codepoint = first & 0x0fu;
        } else if ((first & 0xf8u) == 0xf0u) {
            sequence = 4;
            codepoint = first & 0x07u;
        } else {
            codepoint = RELIEFOS_TEXT_REPLACEMENT_CHAR;
            sequence = 1;
        }
        if (sequence > 1 && pos + sequence > byte_len) {
            codepoint = RELIEFOS_TEXT_REPLACEMENT_CHAR;
            sequence = 1;
        }
        for (uint32_t i = 1; i < sequence; ++i) {
            uint8_t next = (uint8_t)text[pos + i];
            if ((next & 0xc0u) != 0x80u) {
                codepoint = RELIEFOS_TEXT_REPLACEMENT_CHAR;
                sequence = 1;
                break;
            }
            codepoint = (codepoint << 6) | (next & 0x3fu);
        }
        {
            uint32_t width = codepoint < 0x1100u ? 1u : 2u;
            cells += width;
            pixels += width * 8u;
            if (glyphs && count < capacity) {
                glyphs[count] = (struct reliefos_text_glyph){
                    .codepoint = codepoint,
                    .byte_offset = pos,
                    .byte_len = sequence,
                    .cell_width = width,
                    .pixel_width = width * 8u,
                };
            }
            ++count;
        }
        pos += sequence;
    }
    if (out_layout) {
        *out_layout = (struct reliefos_text_layout){
            .text = text,
            .byte_len = byte_len,
            .capacity = capacity,
            .count = count,
            .total_cells = cells,
            .total_px = pixels,
            .glyphs = glyphs,
        };
    }
    return 0;
}

#define HTTP_REQUEST_MAX 1024U

struct libc_http_url {
    char host[RELIEFOS_NET_HOSTNAME_LEN];
    char path[RELIEFOS_NET_HTTP_PATH_LEN];
    uint32_t port;
    uint8_t secure;
};

static char http_tolower(char ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return (char)(ch - 'A' + 'a');
    }
    return ch;
}

static int http_starts_with_ignore_case(const char *text, const char *prefix)
{
    uint32_t i = 0;
    if (!text || !prefix) {
        return 0;
    }
    while (prefix[i]) {
        if (http_tolower(text[i]) != http_tolower(prefix[i])) {
            return 0;
        }
        ++i;
    }
    return 1;
}

static int http_text_eq_ignore_case_n(const char *a, const char *b,
                                      uint32_t len)
{
    for (uint32_t i = 0; i < len; ++i) {
        if (http_tolower(a[i]) != http_tolower(b[i])) {
            return 0;
        }
    }
    return 1;
}

static int http_is_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static int http_is_digit(char ch)
{
    return ch >= '0' && ch <= '9';
}

static int http_is_hex(char ch)
{
    return (ch >= '0' && ch <= '9') ||
           (ch >= 'a' && ch <= 'f') ||
           (ch >= 'A' && ch <= 'F');
}

static uint32_t http_hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return (uint32_t)(ch - '0');
    }
    if (ch >= 'a' && ch <= 'f') {
        return (uint32_t)(ch - 'a' + 10);
    }
    if (ch >= 'A' && ch <= 'F') {
        return (uint32_t)(ch - 'A' + 10);
    }
    return 0;
}

static void http_copy_text(char *dst, uint32_t cap, const char *src)
{
    uint32_t i = 0;
    if (!dst || cap == 0) {
        return;
    }
    while (src && src[i] && i + 1U < cap) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = 0;
}

static void http_copy_bytes(char *dst, uint32_t cap,
                            const char *src, uint32_t len)
{
    uint32_t n = len;
    if (!dst || cap == 0) {
        return;
    }
    if (n + 1U > cap) {
        n = cap - 1U;
    }
    for (uint32_t i = 0; i < n; ++i) {
        dst[i] = src ? src[i] : 0;
    }
    dst[n] = 0;
}

static void http_append_char(char *dst, uint32_t *pos, uint32_t cap, char ch)
{
    if (dst && pos && *pos + 1U < cap) {
        dst[*pos] = ch;
        ++(*pos);
        dst[*pos] = 0;
    }
}

static void http_append_text(char *dst, uint32_t *pos, uint32_t cap,
                             const char *src)
{
    while (src && *src) {
        http_append_char(dst, pos, cap, *src++);
    }
}

static void http_append_u32(char *dst, uint32_t *pos, uint32_t cap,
                            uint32_t value)
{
    char tmp[12];
    uint32_t n = 0;
    if (value == 0) {
        http_append_char(dst, pos, cap, '0');
        return;
    }
    while (value && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (n) {
        http_append_char(dst, pos, cap, tmp[--n]);
    }
}

static int http_parse_url(const char *url, struct libc_http_url *out)
{
    const char *p;
    uint32_t host_pos = 0;
    uint32_t path_pos = 0;
    uint32_t port;
    if (!url || !out) {
        return 0;
    }
    if (http_starts_with_ignore_case(url, "https://")) {
        out->secure = 1;
        port = 443;
        p = url + 8;
    } else if (http_starts_with_ignore_case(url, "http://")) {
        out->secure = 0;
        port = 80;
        p = url + 7;
    } else {
        return 0;
    }
    while (*p && *p != '/' && *p != ':' && *p != '#' && *p != '?' &&
           host_pos + 1U < sizeof(out->host)) {
        out->host[host_pos++] = *p++;
    }
    out->host[host_pos] = 0;
    if (!out->host[0]) {
        return 0;
    }
    if (*p == ':') {
        port = 0;
        ++p;
        while (http_is_digit(*p)) {
            port = port * 10U + (uint32_t)(*p - '0');
            if (port > 65535U) {
                return 0;
            }
            ++p;
        }
        if (port == 0) {
            return 0;
        }
    }
    if (*p == '/') {
        while (*p && *p != '#' && path_pos + 1U < sizeof(out->path)) {
            out->path[path_pos++] = *p++;
        }
    } else if (*p == '?') {
        out->path[path_pos++] = '/';
        while (*p && *p != '#' && path_pos + 1U < sizeof(out->path)) {
            out->path[path_pos++] = *p++;
        }
    }
    if (path_pos == 0) {
        out->path[path_pos++] = '/';
    }
    out->path[path_pos] = 0;
    out->port = port;
    return 1;
}

/* Never silently downgrade a protected request to cleartext after a redirect. */
static int http_url_is_secure(const char *url)
{
    struct libc_http_url parsed;
    return http_parse_url(url, &parsed) && parsed.secure;
}

static void http_build_url(char *dst, uint32_t cap, const char *host,
                           uint32_t port, uint8_t secure, const char *path)
{
    uint32_t pos = 0;
    if (!dst || cap == 0) {
        return;
    }
    dst[0] = 0;
    http_append_text(dst, &pos, cap, secure ? "https://" : "http://");
    http_append_text(dst, &pos, cap, host);
    if (port != (secure ? 443U : 80U)) {
        http_append_char(dst, &pos, cap, ':');
        http_append_u32(dst, &pos, cap, port);
    }
    if (!path || path[0] != '/') {
        http_append_char(dst, &pos, cap, '/');
    }
    http_append_text(dst, &pos, cap, path && path[0] ? path : "/");
}

static void http_parent_path(const char *path, char *dst, uint32_t cap)
{
    uint32_t last_slash = 0;
    uint32_t i = 0;
    if (!dst || cap == 0) {
        return;
    }
    if (!path || !path[0]) {
        http_copy_text(dst, cap, "/");
        return;
    }
    while (path[i] && path[i] != '?' && path[i] != '#') {
        if (path[i] == '/') {
            last_slash = i;
        }
        ++i;
    }
    if (last_slash == 0) {
        http_copy_text(dst, cap, "/");
        return;
    }
    http_copy_bytes(dst, cap, path, last_slash + 1U);
}

int reliefos_http_resolve_url(const char *base_url, const char *location,
                            char *out, uint32_t capacity)
{
    char base_copy[RELIEFOS_HTTP_URL_LEN];
    char location_copy[RELIEFOS_HTTP_URL_LEN];
    const char *base_text = base_url;
    const char *location_text = location;
    struct libc_http_url base;
    char dir[RELIEFOS_NET_HTTP_PATH_LEN];
    uint32_t pos = 0;
    if (!out || capacity == 0) {
        return -1;
    }
    if (base_url == out && base_url) {
        http_copy_text(base_copy, sizeof(base_copy), base_url);
        base_text = base_copy;
    }
    if (location == out && location) {
        http_copy_text(location_copy, sizeof(location_copy), location);
        location_text = location_copy;
    }
    out[0] = 0;
    if (!location_text || !location_text[0]) {
        http_copy_text(out, capacity, base_text);
        return 0;
    }
    if (http_starts_with_ignore_case(location_text, "http://") ||
        http_starts_with_ignore_case(location_text, "https://")) {
        http_copy_text(out, capacity, location_text);
        return 0;
    }
    if (location_text[0] == '/' && location_text[1] == '/') {
        http_append_text(out, &pos, capacity, base_text &&
                         http_starts_with_ignore_case(base_text, "https://")
                             ? "https:"
                             : "http:");
        http_append_text(out, &pos, capacity, location_text);
        return 0;
    }
    if (!http_parse_url(base_text, &base)) {
        http_copy_text(out, capacity, location_text);
        return 0;
    }
    if (location_text[0] == '#') {
        http_copy_text(out, capacity, base_text);
        return 0;
    }
    if (location_text[0] == '/') {
        http_build_url(out, capacity, base.host, base.port, base.secure,
                       location_text);
        return 0;
    }
    http_parent_path(base.path, dir, sizeof(dir));
    out[0] = 0;
    http_append_text(out, &pos, capacity,
                     base.secure ? "https://" : "http://");
    http_append_text(out, &pos, capacity, base.host);
    if (base.port != (base.secure ? 443U : 80U)) {
        http_append_char(out, &pos, capacity, ':');
        http_append_u32(out, &pos, capacity, base.port);
    }
    http_append_text(out, &pos, capacity, dir);
    http_append_text(out, &pos, capacity, location_text);
    return 0;
}

static uint32_t http_find_body_offset(const char *data, uint32_t len)
{
    for (uint32_t i = 0; i + 3U < len; ++i) {
        if (data[i] == '\r' && data[i + 1U] == '\n' &&
            data[i + 2U] == '\r' && data[i + 3U] == '\n') {
            return i + 4U;
        }
    }
    for (uint32_t i = 0; i + 1U < len; ++i) {
        if (data[i] == '\n' && data[i + 1U] == '\n') {
            return i + 2U;
        }
    }
    return 0;
}

static uint32_t http_parse_status_code(const char *headers, uint32_t len)
{
    uint32_t pos = 0;
    uint32_t status = 0;
    if (!headers || len < 12U ||
        headers[0] != 'H' || headers[1] != 'T' ||
        headers[2] != 'T' || headers[3] != 'P' ||
        headers[4] != '/') {
        return 0;
    }
    while (pos < len && headers[pos] != ' ' &&
           headers[pos] != '\r' && headers[pos] != '\n') {
        ++pos;
    }
    while (pos < len && headers[pos] == ' ') {
        ++pos;
    }
    for (uint32_t i = 0; i < 3U && pos < len; ++i, ++pos) {
        if (!http_is_digit(headers[pos])) {
            return 0;
        }
        status = status * 10U + (uint32_t)(headers[pos] - '0');
    }
    return status;
}

static int http_header_value(const char *headers, uint32_t header_len,
                             const char *name, char *out, uint32_t cap)
{
    uint32_t name_len = (uint32_t)strlen(name);
    uint32_t i = 0;
    if (out && cap) {
        out[0] = 0;
    }
    if (!headers || !name || !name[0]) {
        return 0;
    }
    while (i < header_len && headers[i] != '\n') {
        ++i;
    }
    if (i < header_len) {
        ++i;
    }
    while (i < header_len) {
        uint32_t line_start = i;
        uint32_t line_end;
        uint32_t colon = i;
        uint32_t value_start;
        uint32_t value_end;
        while (i < header_len && headers[i] != '\n' && headers[i] != '\r') {
            ++i;
        }
        line_end = i;
        while (i < header_len && (headers[i] == '\r' || headers[i] == '\n')) {
            ++i;
        }
        if (line_end == line_start) {
            break;
        }
        while (colon < line_end && headers[colon] != ':') {
            ++colon;
        }
        if (colon == line_end || colon - line_start != name_len) {
            continue;
        }
        if (!http_text_eq_ignore_case_n(headers + line_start, name, name_len)) {
            continue;
        }
        value_start = colon + 1U;
        while (value_start < line_end && http_is_space(headers[value_start])) {
            ++value_start;
        }
        value_end = line_end;
        while (value_end > value_start && http_is_space(headers[value_end - 1U])) {
            --value_end;
        }
        http_copy_bytes(out, cap, headers + value_start, value_end - value_start);
        return 1;
    }
    return 0;
}

static int http_contains_ignore_case(const char *text, const char *needle)
{
    uint32_t needle_len;
    uint32_t text_len;
    if (!text || !needle) {
        return 0;
    }
    needle_len = (uint32_t)strlen(needle);
    text_len = (uint32_t)strlen(text);
    if (needle_len == 0 || needle_len > text_len) {
        return 0;
    }
    for (uint32_t i = 0; i + needle_len <= text_len; ++i) {
        if (http_text_eq_ignore_case_n(text + i, needle, needle_len)) {
            return 1;
        }
    }
    return 0;
}

static uint32_t http_parse_decimal(const char *text, int *ok)
{
    uint32_t value = 0;
    uint32_t i = 0;
    if (ok) {
        *ok = 0;
    }
    while (text && http_is_space(text[i])) {
        ++i;
    }
    if (!text || !http_is_digit(text[i])) {
        return 0;
    }
    while (http_is_digit(text[i])) {
        value = value * 10U + (uint32_t)(text[i] - '0');
        ++i;
    }
    if (ok) {
        *ok = 1;
    }
    return value;
}

static uint32_t http_decode_chunked(char *buffer, uint32_t body_offset,
                                    uint32_t raw_len, uint32_t capacity,
                                    uint32_t *flags)
{
    uint32_t src = body_offset;
    uint32_t dst = 0;
    while (src < raw_len) {
        uint32_t chunk_size = 0;
        uint32_t saw_hex = 0;
        while (src < raw_len && (buffer[src] == '\r' || buffer[src] == '\n')) {
            ++src;
        }
        while (src < raw_len && http_is_hex(buffer[src])) {
            chunk_size = chunk_size * 16U + http_hex_value(buffer[src]);
            ++src;
            saw_hex = 1;
        }
        if (!saw_hex) {
            break;
        }
        while (src < raw_len && buffer[src] != '\n') {
            ++src;
        }
        if (src < raw_len && buffer[src] == '\n') {
            ++src;
        }
        if (chunk_size == 0) {
            break;
        }
        if (src + chunk_size > raw_len) {
            chunk_size = raw_len > src ? raw_len - src : 0;
            if (flags) {
                *flags |= RELIEFOS_HTTP_FLAG_TRUNCATED;
            }
        }
        for (uint32_t i = 0; i < chunk_size; ++i) {
            if (dst + 1U >= capacity) {
                if (flags) {
                    *flags |= RELIEFOS_HTTP_FLAG_TRUNCATED;
                }
                break;
            }
            buffer[dst++] = buffer[src + i];
        }
        src += chunk_size;
    }
    if (capacity) {
        buffer[dst < capacity ? dst : capacity - 1U] = 0;
    }
    return dst;
}

static uint32_t http_copy_body(char *buffer, uint32_t body_offset,
                               uint32_t raw_len, uint32_t capacity,
                               uint32_t wanted_len, uint32_t *flags)
{
    uint32_t available = body_offset < raw_len ? raw_len - body_offset : 0;
    uint32_t copy_len = available;
    if (wanted_len && wanted_len < copy_len) {
        copy_len = wanted_len;
    }
    if (wanted_len && available < wanted_len && flags) {
        *flags |= RELIEFOS_HTTP_FLAG_TRUNCATED;
    }
    if (copy_len + 1U > capacity) {
        copy_len = capacity ? capacity - 1U : 0;
        if (flags) {
            *flags |= RELIEFOS_HTTP_FLAG_TRUNCATED;
        }
    }
    for (uint32_t i = 0; i < copy_len; ++i) {
        buffer[i] = buffer[body_offset + i];
    }
    if (capacity) {
        buffer[copy_len] = 0;
    }
    return copy_len;
}

static int http_extra_header_present(const char *headers, const char *name)
{
    uint32_t name_len;
    uint32_t i = 0;
    if (!headers || !name || !name[0]) {
        return 0;
    }
    name_len = (uint32_t)strlen(name);
    while (headers[i]) {
        uint32_t line_start = i;
        uint32_t line_end;
        uint32_t colon;
        while (headers[i] && headers[i] != '\r' && headers[i] != '\n') {
            ++i;
        }
        line_end = i;
        colon = line_start;
        while (colon < line_end && headers[colon] != ':') {
            ++colon;
        }
        if (colon < line_end && colon - line_start == name_len &&
            http_text_eq_ignore_case_n(headers + line_start, name, name_len)) {
            return 1;
        }
        while (headers[i] == '\r' || headers[i] == '\n') {
            ++i;
        }
    }
    return 0;
}

static uint32_t http_build_request_text(char *dst, uint32_t cap,
                                        const struct libc_http_url *url,
                                        const struct reliefos_http_request *request)
{
    const char *method = request->method && request->method[0]
                             ? request->method
                             : "GET";
    uint32_t pos = 0;
    if (!dst || !cap || !url) {
        return 0;
    }
    dst[0] = 0;
    http_append_text(dst, &pos, cap, method);
    http_append_char(dst, &pos, cap, ' ');
    http_append_text(dst, &pos, cap, url->path[0] ? url->path : "/");
    http_append_text(dst, &pos, cap, " HTTP/1.1\r\nHost: ");
    http_append_text(dst, &pos, cap, url->host);
    if (url->port != (url->secure ? 443U : 80U)) {
        http_append_char(dst, &pos, cap, ':');
        http_append_u32(dst, &pos, cap, url->port);
    }
    http_append_text(dst, &pos, cap, "\r\n");
    if (!http_extra_header_present(request->extra_headers, "User-Agent")) {
        http_append_text(dst, &pos, cap, "User-Agent: ReliefOS/4\r\n");
    }
    if (!http_extra_header_present(request->extra_headers, "Accept")) {
        http_append_text(dst, &pos, cap, "Accept: */*\r\n");
    }
    if (!http_extra_header_present(request->extra_headers, "Accept-Encoding")) {
        http_append_text(dst, &pos, cap, "Accept-Encoding: identity\r\n");
    }
    if (!http_extra_header_present(request->extra_headers, "Connection")) {
        http_append_text(dst, &pos, cap, "Connection: close\r\n");
    }
    if (request->request_body && request->request_body_len) {
        http_append_text(dst, &pos, cap, "Content-Length: ");
        http_append_u32(dst, &pos, cap, request->request_body_len);
        http_append_text(dst, &pos, cap, "\r\n");
    }
    if (request->extra_headers && request->extra_headers[0]) {
        http_append_text(dst, &pos, cap, request->extra_headers);
        if (pos < 2U || dst[pos - 1U] != '\n') {
            http_append_text(dst, &pos, cap, "\r\n");
        }
    }
    http_append_text(dst, &pos, cap, "\r\n");
    return pos + 1U < cap ? pos : 0;
}

static int http_fetch_once(const char *url_text,
                           const struct reliefos_http_request *request,
                           struct reliefos_http_response *response,
                           char *location, uint32_t location_cap)
{
    struct libc_http_url url;
    struct reliefos_net_socket_connect conn;
    char request_text[HTTP_REQUEST_MAX];
    char transfer_encoding[48];
    char content_length_text[32];
    uint32_t request_len;
    uint32_t net_status = RELIEFOS_NET_STATUS_HTTP_FAILED;
    uint32_t raw_len = 0;
    uint32_t body_offset;
    uint32_t header_len;
    uint32_t timeout_ms = request->timeout_ms ? request->timeout_ms
                                              : RELIEFOS_HTTP_DEFAULT_TIMEOUT_MS;
    char *raw_response = request->response_body;
    uint32_t raw_capacity = request->response_body_capacity;
    int raw_response_owned = 0;
    int socket;
    int ret;

    if (location && location_cap) {
        location[0] = 0;
    }
    if (!http_starts_with_ignore_case(url_text, "http://") &&
        !http_starts_with_ignore_case(url_text, "https://")) {
        printf("[http] request rejected unsupported scheme\n");
        response->net_status = RELIEFOS_NET_STATUS_PROTOCOL_UNSUPPORTED;
        return 0;
    }
    if (!http_parse_url(url_text, &url)) {
        printf("[http] request rejected invalid url\n");
        response->net_status = RELIEFOS_NET_STATUS_BAD_ARGUMENT;
        return 0;
    }
    printf("[http] request host=%s port=%u secure=%u timeout=%u\n", url.host,
           url.port, url.secure, timeout_ms);
    request_len = http_build_request_text(request_text, sizeof(request_text),
                                          &url, request);
    if (!request_len) {
        printf("[http] request build failed host=%s\n", url.host);
        response->net_status = RELIEFOS_NET_STATUS_BAD_ARGUMENT;
        return 0;
    }
    socket = reliefos_socket_tcp();
    if (socket < 0) {
        printf("[http] socket open failed host=%s\n", url.host);
        response->net_status = RELIEFOS_NET_STATUS_SOCKET_LIMIT;
        return 0;
    }
    ret = reliefos_socket_connect(socket, url.host, url.port, timeout_ms, &conn);
    if (ret < 0 || conn.status != RELIEFOS_NET_STATUS_OK) {
        printf("[http] connect failed host=%s ret=%d status=%u\n", url.host,
               ret, conn.status);
        reliefos_socket_close(socket);
        response->net_status = ret < 0 ? RELIEFOS_NET_STATUS_TCP_FAILED
                                       : conn.status;
        return 0;
    }
    printf("[http] connected host=%s socket=%d remote_ip=%u\n", url.host,
           socket, conn.remote_ip);
    if (url.secure) {
        if (raw_capacity > UINT32_MAX - RELIEFOS_HTTP_HEADER_MAX - 1U) {
            reliefos_socket_close(socket);
            response->net_status = RELIEFOS_NET_STATUS_HTTP_TOO_LARGE;
            return 0;
        }
        raw_capacity += RELIEFOS_HTTP_HEADER_MAX + 1U;
        raw_response = malloc(raw_capacity);
        if (!raw_response) {
            reliefos_socket_close(socket);
            response->net_status = RELIEFOS_NET_STATUS_SOCKET_LIMIT;
            return 0;
        }
        raw_response_owned = 1;
        if (reliefos_tls_http_exchange(socket, url.host, timeout_ms,
                                     request_text, request_len,
                                     request->request_body,
                                     request->request_body_len,
                                     raw_response, raw_capacity,
                                     &raw_len) < 0) {
            printf("[http] TLS exchange failed host=%s raw=%u\n", url.host,
                   raw_len);
            reliefos_socket_close(socket);
            free(raw_response);
            response->net_status = RELIEFOS_NET_STATUS_TLS_FAILED;
            return 0;
        }
        net_status = RELIEFOS_NET_STATUS_OK;
        printf("[http] TLS exchange complete host=%s raw=%u\n", url.host,
               raw_len);
    } else {
        ret = (int)reliefos_socket_send(socket, request_text, request_len,
                                      timeout_ms, &net_status);
        if (ret < 0 || net_status != RELIEFOS_NET_STATUS_OK ||
            (uint32_t)ret != request_len) {
            reliefos_socket_close(socket);
            response->net_status = net_status;
            return 0;
        }
        if (request->request_body && request->request_body_len) {
            ret = (int)reliefos_socket_send(socket, request->request_body,
                                          request->request_body_len,
                                          timeout_ms, &net_status);
            if (ret < 0 || net_status != RELIEFOS_NET_STATUS_OK ||
                (uint32_t)ret != request->request_body_len) {
                reliefos_socket_close(socket);
                response->net_status = net_status;
                return 0;
            }
        }
        while (raw_len + 1U < raw_capacity) {
            long got = reliefos_socket_recv(socket,
                                          raw_response + raw_len,
                                          raw_capacity - raw_len - 1U,
                                          raw_len ? 1200U : timeout_ms,
                                          &net_status);
            if (got < 0) {
                printf("[http] response read failed host=%s ret=%ld status=%u raw=%u\n",
                       url.host, got, net_status, raw_len);
                reliefos_socket_close(socket);
                if (raw_response_owned) free(raw_response);
                response->net_status = RELIEFOS_NET_STATUS_TCP_FAILED;
                return 0;
            }
            if (got == 0) {
                if (net_status == RELIEFOS_NET_STATUS_OK ||
                    (net_status == RELIEFOS_NET_STATUS_TCP_TIMEOUT && raw_len)) {
                    net_status = RELIEFOS_NET_STATUS_OK;
                }
                break;
            }
            raw_len += (uint32_t)got;
        }
    }
    reliefos_socket_close(socket);
    raw_response[raw_len] = 0;
    response->net_status = net_status;
    if (raw_len + 1U >= raw_capacity) {
        response->flags |= RELIEFOS_HTTP_FLAG_TRUNCATED;
    }
    if (net_status != RELIEFOS_NET_STATUS_OK) {
        printf("[http] response transport failed host=%s status=%u raw=%u\n",
               url.host, net_status, raw_len);
        if (raw_response_owned) free(raw_response);
        return 0;
    }
    body_offset = http_find_body_offset(raw_response, raw_len);
    if (!body_offset) {
        printf("[http] response missing header terminator host=%s raw=%u\n",
               url.host, raw_len);
        response->net_status = RELIEFOS_NET_STATUS_HTTP_FAILED;
        if (raw_response_owned) free(raw_response);
        return 0;
    }
    header_len = body_offset;
    response->http_status = http_parse_status_code(raw_response,
                                                   header_len);
    if (!response->http_status) {
        printf("[http] response status parse failed host=%s headers=%u raw=%u\n",
               url.host, header_len, raw_len);
        response->net_status = RELIEFOS_NET_STATUS_HTTP_FAILED;
        if (raw_response_owned) free(raw_response);
        return 0;
    }
    response->headers_len = header_len;
    if (request->response_headers && request->response_headers_capacity) {
        if (header_len + 1U > request->response_headers_capacity) {
            response->headers_len = request->response_headers_capacity - 1U;
            response->flags |= RELIEFOS_HTTP_FLAG_TRUNCATED;
        }
        http_copy_bytes(request->response_headers,
                        request->response_headers_capacity,
                        raw_response,
                        response->headers_len);
    }
    http_header_value(raw_response, header_len, "Content-Type",
                      response->content_type, sizeof(response->content_type));
    http_header_value(raw_response, header_len, "Location",
                      location, location_cap);
    transfer_encoding[0] = 0;
    http_header_value(raw_response, header_len, "Transfer-Encoding",
                      transfer_encoding, sizeof(transfer_encoding));
    content_length_text[0] = 0;
    if (http_header_value(raw_response, header_len, "Content-Length",
                          content_length_text, sizeof(content_length_text))) {
        int ok = 0;
        response->content_length = http_parse_decimal(content_length_text, &ok);
        if (ok) {
            response->flags |= RELIEFOS_HTTP_FLAG_CONTENT_LENGTH;
        }
    }
    if (http_contains_ignore_case(transfer_encoding, "chunked")) {
        response->flags |= RELIEFOS_HTTP_FLAG_CHUNKED;
        response->body_len = http_decode_chunked(raw_response,
                                                body_offset, raw_len,
                                                raw_capacity,
                                                &response->flags);
    } else {
        response->body_len = http_copy_body(raw_response,
                                           body_offset, raw_len,
                                           raw_capacity,
                                           response->content_length,
                                           &response->flags);
    }
    if (raw_response_owned) {
        uint32_t body_len = response->body_len;
        if (body_len + 1U > request->response_body_capacity) {
            body_len = request->response_body_capacity
                           ? request->response_body_capacity - 1U : 0;
            response->flags |= RELIEFOS_HTTP_FLAG_TRUNCATED;
        }
        if (body_len) memcpy(request->response_body, raw_response, body_len);
        if (request->response_body_capacity) request->response_body[body_len] = 0;
        response->body_len = body_len;
        free(raw_response);
    }
    printf("[http] response host=%s status=%u headers=%u raw=%u body=%u length=%u flags=0x%x type=%s\n",
           url.host, response->http_status, response->headers_len, raw_len,
           response->body_len, response->content_length, response->flags,
           response->content_type[0] ? response->content_type : "(none)");
    return 0;
}

static int http_is_redirect(uint32_t status)
{
    return status == 301U || status == 302U || status == 303U ||
           status == 307U || status == 308U;
}

int reliefos_http_request(const struct reliefos_http_request *request,
                        struct reliefos_http_response *response)
{
    char current_url[RELIEFOS_HTTP_URL_LEN];
    char location[RELIEFOS_HTTP_URL_LEN];
    char next_url[RELIEFOS_HTTP_URL_LEN];
    uint32_t max_redirects;
    if (!request || !response || !request->url ||
        !request->response_body || request->response_body_capacity == 0) {
        return -1;
    }
    *response = (struct reliefos_http_response){0};
    max_redirects = request->max_redirects == RELIEFOS_HTTP_NO_REDIRECTS
                        ? 0
                        : request->max_redirects
                              ? request->max_redirects
                              : RELIEFOS_HTTP_DEFAULT_REDIRECTS;
    http_copy_text(current_url, sizeof(current_url), request->url);
    http_copy_text(response->final_url, sizeof(response->final_url),
                   current_url);
    for (;;) {
        location[0] = 0;
        uint32_t preserved_flags =
            response->flags & RELIEFOS_HTTP_FLAG_REDIRECTED;
        uint32_t redirect_count = response->redirect_count;
        response->net_status = RELIEFOS_NET_STATUS_HTTP_FAILED;
        response->http_status = 0;
        response->flags = preserved_flags;
        response->body_len = 0;
        response->headers_len = 0;
        response->content_length = 0;
        response->redirect_count = redirect_count;
        response->content_type[0] = 0;
        http_fetch_once(current_url, request, response,
                        location, sizeof(location));
        http_copy_text(response->final_url, sizeof(response->final_url),
                       current_url);
        printf("[http] request result url=%s status=%u net=%u body=%u redirects=%u flags=0x%x\n",
               current_url, response->http_status, response->net_status,
               response->body_len, response->redirect_count, response->flags);
        if (response->net_status != RELIEFOS_NET_STATUS_OK ||
            !http_is_redirect(response->http_status) ||
            !location[0]) {
            return 0;
        }
        printf("[http] redirect from=%s to=(location present) status=%u\n",
               current_url, response->http_status);
        if (max_redirects == 0) {
            return 0;
        }
        if (response->redirect_count >= max_redirects) {
            response->net_status = RELIEFOS_NET_STATUS_HTTP_FAILED;
            return 0;
        }
        if (reliefos_http_resolve_url(current_url, location,
                                    next_url, sizeof(next_url)) < 0) {
            response->net_status = RELIEFOS_NET_STATUS_HTTP_FAILED;
            return 0;
        }
        if (http_url_is_secure(current_url) &&
            !http_url_is_secure(next_url)) {
            printf("[http] rejected HTTPS downgrade redirect\n");
            response->net_status = RELIEFOS_NET_STATUS_TLS_FAILED;
            return 0;
        }
        http_copy_text(current_url, sizeof(current_url), next_url);
        ++response->redirect_count;
        response->flags |= RELIEFOS_HTTP_FLAG_REDIRECTED;
    }
}

int reliefos_http_get(const char *url, uint32_t timeout_ms,
                    char *response_body, uint32_t response_body_capacity,
                    char *response_headers, uint32_t response_headers_capacity,
                    struct reliefos_http_response *response)
{
    struct reliefos_http_request request = {
        .url = url,
        .method = "GET",
        .extra_headers = 0,
        .request_body = 0,
        .request_body_len = 0,
        .timeout_ms = timeout_ms,
        .max_redirects = RELIEFOS_HTTP_DEFAULT_REDIRECTS,
        .response_body = response_body,
        .response_body_capacity = response_body_capacity,
        .response_headers = response_headers,
        .response_headers_capacity = response_headers_capacity,
    };
    return reliefos_http_request(&request, response);
}

#define HTTP_DOWNLOAD_HEADER_MAX RELIEFOS_HTTP_HEADER_MAX
#define HTTP_DOWNLOAD_READ_SIZE 4096U

enum http_download_body_state {
    HTTP_DOWNLOAD_BODY_IDENTITY,
    HTTP_DOWNLOAD_BODY_CHUNK_SIZE,
    HTTP_DOWNLOAD_BODY_CHUNK_DATA,
    HTTP_DOWNLOAD_BODY_CHUNK_CRLF,
};

struct http_download_stream {
    char headers[HTTP_DOWNLOAD_HEADER_MAX + 1U];
    char location[RELIEFOS_HTTP_URL_LEN];
    struct reliefos_http_response *response;
    reliefos_http_download_progress_fn progress;
    void *progress_context;
    uint32_t headers_len;
    uint32_t total;
    uint32_t received;
    uint32_t chunk_remaining;
    uint8_t body_state;
    uint8_t chunk_seen_hex;
    uint8_t chunk_extension;
    uint8_t chunk_crlf_seen;
    uint8_t headers_ready;
    uint8_t complete;
    uint8_t redirect;
    uint8_t failed;
    uint8_t cancelled;
    int fd;
};

static int http_download_report(struct http_download_stream *stream)
{
    if (!stream || !stream->progress) {
        return 0;
    }
    if (stream->progress(stream->received, stream->total,
                         stream->progress_context) < 0) {
        stream->cancelled = 1;
        return -1;
    }
    return 0;
}

static int http_download_write_all(struct http_download_stream *stream,
                                   const char *data, uint32_t length)
{
    uint32_t written = 0;
    if (!stream || !data) {
        return -1;
    }
    if (stream->total && (length > stream->total ||
                          stream->received > stream->total - length)) {
        stream->failed = 1;
        return -1;
    }
    while (written < length) {
        long ret = write(stream->fd, data + written, length - written);
        if (ret <= 0) {
            stream->failed = 1;
            return -1;
        }
        written += (uint32_t)ret;
    }
    stream->received += length;
    if (http_download_report(stream) < 0) {
        return -1;
    }
    if (stream->total && stream->received == stream->total) {
        stream->complete = 1;
        return -1;
    }
    return 0;
}

static int http_download_parse_headers(struct http_download_stream *stream)
{
    char transfer_encoding[48];
    char content_length_text[32];
    int valid_length = 0;
    if (!stream || !stream->response) {
        return -1;
    }
    stream->response->headers_len = stream->headers_len;
    stream->response->http_status = http_parse_status_code(stream->headers,
                                                            stream->headers_len);
    if (!stream->response->http_status) {
        stream->failed = 1;
        return -1;
    }
    stream->response->net_status = RELIEFOS_NET_STATUS_OK;
    http_header_value(stream->headers, stream->headers_len, "Content-Type",
                      stream->response->content_type,
                      sizeof(stream->response->content_type));
    http_header_value(stream->headers, stream->headers_len, "Location",
                      stream->location, sizeof(stream->location));
    transfer_encoding[0] = 0;
    http_header_value(stream->headers, stream->headers_len,
                      "Transfer-Encoding", transfer_encoding,
                      sizeof(transfer_encoding));
    content_length_text[0] = 0;
    if (http_header_value(stream->headers, stream->headers_len,
                          "Content-Length", content_length_text,
                          sizeof(content_length_text))) {
        stream->response->content_length =
            http_parse_decimal(content_length_text, &valid_length);
        if (valid_length) {
            stream->response->flags |= RELIEFOS_HTTP_FLAG_CONTENT_LENGTH;
        }
    }
    if (http_contains_ignore_case(transfer_encoding, "chunked")) {
        stream->response->flags |= RELIEFOS_HTTP_FLAG_CHUNKED;
        stream->body_state = HTTP_DOWNLOAD_BODY_CHUNK_SIZE;
    } else {
        stream->body_state = HTTP_DOWNLOAD_BODY_IDENTITY;
        stream->total = valid_length ? stream->response->content_length : 0;
    }
    stream->headers_ready = 1;
    if (http_is_redirect(stream->response->http_status) && stream->location[0]) {
        stream->redirect = 1;
        return -1;
    }
    if (stream->response->http_status < 200U ||
        stream->response->http_status >= 300U) {
        stream->failed = 1;
        return -1;
    }
    if (http_download_report(stream) < 0) {
        return -1;
    }
    if (stream->total == 0 &&
        (stream->response->flags & RELIEFOS_HTTP_FLAG_CONTENT_LENGTH)) {
        stream->complete = 1;
        return -1;
    }
    return 0;
}

static int http_download_consume_chunked(struct http_download_stream *stream,
                                         const char *data, uint32_t length,
                                         uint32_t *position)
{
    while (*position < length) {
        char ch = data[*position];
        if (stream->body_state == HTTP_DOWNLOAD_BODY_CHUNK_SIZE) {
            ++(*position);
            if (ch == '\r') {
                continue;
            }
            if (ch == '\n') {
                if (!stream->chunk_seen_hex) {
                    stream->failed = 1;
                    return -1;
                }
                if (stream->chunk_remaining == 0) {
                    stream->complete = 1;
                    return -1;
                }
                stream->body_state = HTTP_DOWNLOAD_BODY_CHUNK_DATA;
                continue;
            }
            if (ch == ';') {
                if (!stream->chunk_seen_hex) {
                    stream->failed = 1;
                    return -1;
                }
                stream->chunk_extension = 1;
                continue;
            }
            if (!stream->chunk_extension && http_is_hex(ch)) {
                uint32_t digit = http_hex_value(ch);
                if (stream->chunk_remaining > (0xffffffffU - digit) / 16U) {
                    stream->failed = 1;
                    return -1;
                }
                stream->chunk_remaining = stream->chunk_remaining * 16U + digit;
                stream->chunk_seen_hex = 1;
                continue;
            }
            if (!stream->chunk_extension) {
                stream->failed = 1;
                return -1;
            }
            continue;
        }
        if (stream->body_state == HTTP_DOWNLOAD_BODY_CHUNK_DATA) {
            uint32_t available = length - *position;
            uint32_t take = available < stream->chunk_remaining
                                ? available : stream->chunk_remaining;
            if (http_download_write_all(stream, data + *position, take) < 0) {
                return -1;
            }
            *position += take;
            stream->chunk_remaining -= take;
            if (stream->chunk_remaining == 0) {
                stream->body_state = HTTP_DOWNLOAD_BODY_CHUNK_CRLF;
                stream->chunk_crlf_seen = 0;
            }
            continue;
        }
        ++(*position);
        if (ch == '\r' && !stream->chunk_crlf_seen) {
            stream->chunk_crlf_seen = 1;
            continue;
        }
        if (ch == '\n') {
            stream->body_state = HTTP_DOWNLOAD_BODY_CHUNK_SIZE;
            stream->chunk_remaining = 0;
            stream->chunk_seen_hex = 0;
            stream->chunk_extension = 0;
            continue;
        }
        stream->failed = 1;
        return -1;
    }
    return 0;
}

static int http_download_stream_data(const void *raw_data, uint32_t length,
                                     void *context)
{
    struct http_download_stream *stream = (struct http_download_stream *)context;
    const char *data = (const char *)raw_data;
    uint32_t position = 0;
    if (!stream) {
        return -1;
    }
    if (length == 0) {
        return http_download_report(stream);
    }
    while (position < length) {
        if (!stream->headers_ready) {
            uint32_t body_offset;
            if (stream->headers_len >= HTTP_DOWNLOAD_HEADER_MAX) {
                stream->failed = 1;
                return -1;
            }
            stream->headers[stream->headers_len++] = data[position++];
            stream->headers[stream->headers_len] = 0;
            body_offset = http_find_body_offset(stream->headers,
                                                stream->headers_len);
            if (!body_offset) {
                continue;
            }
            if (http_download_parse_headers(stream) < 0) {
                return -1;
            }
            continue;
        }
        if (stream->body_state == HTTP_DOWNLOAD_BODY_IDENTITY) {
            if (http_download_write_all(stream, data + position,
                                        length - position) < 0) {
                return -1;
            }
            position = length;
        } else if (http_download_consume_chunked(stream, data, length,
                                                  &position) < 0) {
            return -1;
        }
    }
    return 0;
}

static int http_download_fetch_once(const char *url_text,
                                    uint32_t timeout_ms,
                                    struct http_download_stream *stream)
{
    struct libc_http_url url;
    struct reliefos_net_socket_connect connection;
    struct reliefos_http_request request = {0};
    char request_text[HTTP_REQUEST_MAX];
    uint32_t request_len;
    uint32_t net_status = RELIEFOS_NET_STATUS_HTTP_FAILED;
    int socket;
    int ret = -1;
    if (!url_text || !stream || !stream->response ||
        !http_parse_url(url_text, &url)) {
        return -1;
    }
    request.method = "GET";
    request_len = http_build_request_text(request_text, sizeof(request_text),
                                          &url, &request);
    if (!request_len || http_download_report(stream) < 0) {
        return -1;
    }
    socket = reliefos_socket_tcp();
    if (socket < 0) {
        stream->response->net_status = RELIEFOS_NET_STATUS_SOCKET_LIMIT;
        return -1;
    }
    ret = reliefos_socket_connect(socket, url.host, url.port, timeout_ms,
                                &connection);
    if (ret < 0 || connection.status != RELIEFOS_NET_STATUS_OK) {
        stream->response->net_status = ret < 0 ? RELIEFOS_NET_STATUS_TCP_FAILED
                                                : connection.status;
        reliefos_socket_close(socket);
        return -1;
    }
    if (url.secure) {
        ret = reliefos_tls_http_stream(socket, url.host, timeout_ms,
                                     request_text, request_len, 0, 0,
                                     http_download_stream_data, stream);
    } else {
        ret = (int)reliefos_socket_send(socket, request_text, request_len,
                                      timeout_ms, &net_status);
        if (ret >= 0 && net_status == RELIEFOS_NET_STATUS_OK &&
            (uint32_t)ret == request_len) {
            char buffer[HTTP_DOWNLOAD_READ_SIZE];
            unsigned long last_data = reliefos_uptime_ms();
            ret = 0;
            for (;;) {
                long got;
                if (http_download_report(stream) < 0) {
                    ret = -1;
                    break;
                }
                got = reliefos_socket_recv(socket, buffer, sizeof(buffer),
                                         200U,
                                         &net_status);
                if (got == 0) {
                    if (net_status == RELIEFOS_NET_STATUS_TCP_TIMEOUT) {
                        if (reliefos_uptime_ms() - last_data < timeout_ms) {
                            continue;
                        }
                    }
                    break;
                }
                if (got < 0 || http_download_stream_data(buffer,
                                                          (uint32_t)got,
                                                          stream) < 0) {
                    ret = -1;
                    break;
                }
                last_data = reliefos_uptime_ms();
            }
            if (net_status != RELIEFOS_NET_STATUS_OK &&
                !stream->complete && !stream->redirect && !stream->failed &&
                !stream->cancelled) {
                ret = -1;
            }
        } else {
            stream->response->net_status = net_status;
            ret = -1;
        }
    }
    reliefos_socket_close(socket);
    if (stream->redirect || stream->complete) {
        return 0;
    }
    if (ret < 0 || stream->failed || stream->cancelled ||
        !stream->headers_ready) {
        if (stream->response->net_status == RELIEFOS_NET_STATUS_HTTP_FAILED) {
            stream->response->net_status = url.secure
                                               ? RELIEFOS_NET_STATUS_TLS_FAILED
                                               : RELIEFOS_NET_STATUS_TCP_FAILED;
        }
        return -1;
    }
    if (stream->body_state == HTTP_DOWNLOAD_BODY_IDENTITY &&
        (!stream->total || stream->received == stream->total)) {
        stream->complete = 1;
        return 0;
    }
    stream->response->net_status = RELIEFOS_NET_STATUS_HTTP_FAILED;
    return -1;
}

static int http_download_temp_path(const char *output_path, char *temp_path,
                                   uint32_t capacity)
{
    uint32_t length;
    if (!output_path || !output_path[0] || !temp_path || capacity == 0) {
        return 0;
    }
    length = (uint32_t)strlen(output_path);
    if (length + 6U >= capacity) {
        return 0;
    }
    memcpy(temp_path, output_path, length);
    memcpy(temp_path + length, ".part", 6U);
    return 1;
}

int reliefos_http_download(const char *url, const char *output_path,
                         uint32_t timeout_ms,
                         reliefos_http_download_progress_fn progress,
                         void *context,
                         struct reliefos_http_response *response)
{
    char current_url[RELIEFOS_HTTP_URL_LEN];
    char next_url[RELIEFOS_HTTP_URL_LEN];
    char temp_path[RELIEFOS_FS_PATH_LEN];
    uint32_t redirects = 0;
    if (!url || !output_path || !response ||
        !http_download_temp_path(output_path, temp_path, sizeof(temp_path))) {
        return -1;
    }
    *response = (struct reliefos_http_response){0};
    http_copy_text(current_url, sizeof(current_url), url);
    for (;;) {
        struct http_download_stream stream = {0};
        stream.response = response;
        stream.progress = progress;
        stream.progress_context = context;
        stream.fd = open(temp_path, RELIEFOS_O_WRONLY | RELIEFOS_O_CREAT |
                         RELIEFOS_O_TRUNC, 0666);
        if (stream.fd < 0) {
            response->net_status = RELIEFOS_NET_STATUS_HTTP_FAILED;
            return -1;
        }
        response->net_status = RELIEFOS_NET_STATUS_HTTP_FAILED;
        response->http_status = 0;
        response->flags &= RELIEFOS_HTTP_FLAG_REDIRECTED;
        response->body_len = 0;
        response->headers_len = 0;
        response->content_length = 0;
        response->content_type[0] = 0;
        (void)http_download_fetch_once(current_url,
                                       timeout_ms ? timeout_ms
                                                  : RELIEFOS_HTTP_DEFAULT_TIMEOUT_MS,
                                       &stream);
        close(stream.fd);
        http_copy_text(response->final_url, sizeof(response->final_url),
                       current_url);
        if (stream.redirect) {
            unlink(temp_path);
            if (redirects >= RELIEFOS_HTTP_DEFAULT_REDIRECTS ||
                reliefos_http_resolve_url(current_url, stream.location,
                                        next_url, sizeof(next_url)) < 0) {
                response->net_status = RELIEFOS_NET_STATUS_HTTP_FAILED;
                return -1;
            }
            if (http_url_is_secure(current_url) &&
                !http_url_is_secure(next_url)) {
                printf("[http] rejected HTTPS download downgrade redirect\n");
                response->net_status = RELIEFOS_NET_STATUS_TLS_FAILED;
                return -1;
            }
            http_copy_text(current_url, sizeof(current_url), next_url);
            ++redirects;
            response->redirect_count = redirects;
            response->flags |= RELIEFOS_HTTP_FLAG_REDIRECTED;
            continue;
        }
        if (!stream.complete || stream.failed || stream.cancelled ||
            response->net_status != RELIEFOS_NET_STATUS_OK) {
            unlink(temp_path);
            return -1;
        }
        response->body_len = stream.received;
        if (rename(temp_path, output_path) < 0) {
            unlink(temp_path);
            response->net_status = RELIEFOS_NET_STATUS_HTTP_FAILED;
            return -1;
        }
        if (http_download_report(&stream) < 0) {
            return -1;
        }
        return 0;
    }
}

static void libc_copy_fixed(char *dst, uint32_t cap, const char *src)
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

static void libc_clear_secret(void *data, uint32_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)data;
    while (p && len) {
        *p++ = 0;
        --len;
    }
}

int reliefos_system_reboot(void)
{
    return reliefos_auth_request_power(RB_AUTOBOOT);
}

int reliefos_system_shutdown(void)
{
    return reliefos_auth_request_power(RB_POWER_OFF);
}

int reliefos_kernel_debug_get_state(uint32_t *flags)
{
    char buffer[128] = {0};
    int fd;
    if (!flags) return -1;
    *flags = 0;
    fd = open(RELIEFOS_PATH_KERNELDEBUG_CONTROL, RELIEFOS_O_RDONLY, 0);
    if (fd < 0) return 0;
    {
        long got = read(fd, buffer, sizeof(buffer) - 1u);
        if (got > 0) {
            if (buffer[0] == '1') *flags |= RELIEFOS_KERNEL_DEBUG_STATE_ENABLED;
            for (long i = 0; i + 1 < got; ++i) {
                if (buffer[i] == 'a' && buffer[i+1] == 'r' && buffer[i+2] == 'm') {
                    *flags |= RELIEFOS_KERNEL_DEBUG_STATE_NEXT_BOOT;
                }
            }
        }
    }
    close(fd);
    return 0;
}

static int reliefos_kernel_debug_write(const char *text)
{
    int fd = open(RELIEFOS_PATH_KERNELDEBUG_CONTROL,
                  RELIEFOS_O_WRONLY | RELIEFOS_O_CREAT | RELIEFOS_O_TRUNC, 0666);
    uint32_t len = 0;
    if (fd < 0) return fd;
    while (text && text[len]) ++len;
    {
        long wrote = write(fd, text, len);
        close(fd);
        return wrote == (long)len ? 0 : -1;
    }
}

int reliefos_kernel_debug_set_enabled(int enabled)
{
    return reliefos_kernel_debug_write(enabled ? "1\n" : "0\n");
}

int reliefos_kernel_debug_arm_next_boot(void)
{
    return reliefos_kernel_debug_write("arm\n");
}

int reliefos_kernel_debug_clear(void)
{
    return reliefos_kernel_debug_write("");
}

/* Published libleonos.so.2 aliases; keep these in the defining translation unit. */
extern __typeof__(reliefos_fs_acl_get) leonos_fs_acl_get __attribute__((alias("reliefos_fs_acl_get")));
extern __typeof__(reliefos_fs_acl_repair) leonos_fs_acl_repair __attribute__((alias("reliefos_fs_acl_repair")));
extern __typeof__(reliefos_fs_acl_set) leonos_fs_acl_set __attribute__((alias("reliefos_fs_acl_set")));
extern __typeof__(reliefos_fs_acl_take_ownership) leonos_fs_acl_take_ownership __attribute__((alias("reliefos_fs_acl_take_ownership")));
extern __typeof__(reliefos_fstat_legacy) leonos_fstat_legacy __attribute__((alias("reliefos_fstat_legacy")));
extern __typeof__(reliefos_http_download) leonos_http_download __attribute__((alias("reliefos_http_download")));
extern __typeof__(reliefos_http_get) leonos_http_get __attribute__((alias("reliefos_http_get")));
extern __typeof__(reliefos_http_request) leonos_http_request __attribute__((alias("reliefos_http_request")));
extern __typeof__(reliefos_http_resolve_url) leonos_http_resolve_url __attribute__((alias("reliefos_http_resolve_url")));
extern __typeof__(reliefos_kernel_debug_arm_next_boot) leonos_kernel_debug_arm_next_boot __attribute__((alias("reliefos_kernel_debug_arm_next_boot")));
extern __typeof__(reliefos_kernel_debug_clear) leonos_kernel_debug_clear __attribute__((alias("reliefos_kernel_debug_clear")));
extern __typeof__(reliefos_kernel_debug_get_state) leonos_kernel_debug_get_state __attribute__((alias("reliefos_kernel_debug_get_state")));
extern __typeof__(reliefos_kernel_debug_set_enabled) leonos_kernel_debug_set_enabled __attribute__((alias("reliefos_kernel_debug_set_enabled")));
extern __typeof__(reliefos_stat_legacy) leonos_stat_legacy __attribute__((alias("reliefos_stat_legacy")));
extern __typeof__(reliefos_system_reboot) leonos_system_reboot __attribute__((alias("reliefos_system_reboot")));
extern __typeof__(reliefos_system_shutdown) leonos_system_shutdown __attribute__((alias("reliefos_system_shutdown")));
extern __typeof__(reliefos_text_layout_utf8) leonos_text_layout_utf8 __attribute__((alias("reliefos_text_layout_utf8")));
