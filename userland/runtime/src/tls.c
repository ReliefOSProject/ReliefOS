#include <reliefos/fs.h>
#include <reliefos/gui.h>
#include <reliefos/net.h>
#include <reliefos/stdio.h>
#include <reliefos/system.h>
#include <reliefos/syscall.h>
#include <reliefos/tls.h>
#include <reliefos/layout.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <string.h>

#define RELIEFOS_TLS_CA_BUNDLE RELIEFOS_PATH_CACERT
#define RELIEFOS_TLS_CA_BUNDLE_MAX (512U * 1024U)

struct reliefos_tls_io {
    int socket;
    uint32_t timeout_ms;
    reliefos_tls_stream_callback activity;
    void *activity_context;
};

static mbedtls_x509_crt reliefos_tls_roots;
static int reliefos_tls_roots_state;

static uint32_t reliefos_tls_root_count(void)
{
    const mbedtls_x509_crt *root = &reliefos_tls_roots;
    uint32_t count = 0;
    while (root && root->version != 0) {
        ++count;
        root = root->next;
    }
    return count;
}

static void reliefos_tls_log_verify_failure(const char *hostname, uint32_t flags)
{
    printf("[tls] verify failed host=%s flags=0x%x expired=%u future=%u cn=%u trust=%u key=%u md=%u\n",
           hostname ? hostname : "(null)", flags,
           (flags & MBEDTLS_X509_BADCERT_EXPIRED) != 0,
           (flags & MBEDTLS_X509_BADCERT_FUTURE) != 0,
           (flags & MBEDTLS_X509_BADCERT_CN_MISMATCH) != 0,
           (flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED) != 0,
           (flags & (MBEDTLS_X509_BADCERT_BAD_PK | MBEDTLS_X509_BADCERT_BAD_KEY)) != 0,
           (flags & MBEDTLS_X509_BADCERT_BAD_MD) != 0);
}

static int reliefos_tls_is_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int reliefos_tls_header_name_equal(const char *line, const char *name)
{
    while (*name && *line) {
        char left = *line++;
        char right = *name++;
        if (left >= 'a' && left <= 'z') {
            left = (char)(left - ('a' - 'A'));
        }
        if (right >= 'a' && right <= 'z') {
            right = (char)(right - ('a' - 'A'));
        }
        if (left != right) {
            return 0;
        }
    }
    return *name == 0 && *line == ':';
}

static int reliefos_tls_parse_content_length(const char *data,
                                           uint32_t header_len,
                                           uint32_t *content_length)
{
    uint32_t line_start = 0;
    while (line_start < header_len) {
        uint32_t line_end = line_start;
        int ok = 0;
        while (line_end < header_len && data[line_end] != '\n') {
            ++line_end;
        }
        if (reliefos_tls_header_name_equal(data + line_start,
                                         "Content-Length")) {
            const char *value = data + line_start + 15U;
            while (value < data + line_end &&
                   (*value == ':' || *value == ' ' || *value == '\t')) {
                ++value;
            }
            *content_length = 0;
            while (value < data + line_end && *value >= '0' && *value <= '9') {
                *content_length = *content_length * 10U +
                                  (uint32_t)(*value - '0');
                ++value;
                ok = 1;
            }
            return ok;
        }
        line_start = line_end + 1U;
    }
    return 0;
}

int64_t reliefos_mbedtls_time(int64_t *seconds)
{
    struct reliefos_time_info info;
    int64_t value = 0;
    if (reliefos_time_info(&info) == 0 && info.valid) {
        value = (int64_t)info.unix_seconds;
    }
    if (seconds) {
        *seconds = value;
    }
    return value;
}

struct tm *mbedtls_platform_gmtime_r(const int64_t *seconds, struct tm *out)
{
    static const int month_days[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };
    int64_t days;
    int64_t remainder;
    int year = 1970;
    int month = 0;
    if (!seconds || !out || *seconds < 0) {
        return 0;
    }
    days = *seconds / 86400;
    remainder = *seconds % 86400;
    while (days >= (reliefos_tls_is_leap_year(year) ? 366 : 365)) {
        days -= reliefos_tls_is_leap_year(year) ? 366 : 365;
        ++year;
    }
    while (month < 12) {
        int count = month_days[month];
        if (month == 1 && reliefos_tls_is_leap_year(year)) {
            ++count;
        }
        if (days < count) {
            break;
        }
        days -= count;
        ++month;
    }
    out->tm_sec = (int)(remainder % 60);
    out->tm_min = (int)((remainder / 60) % 60);
    out->tm_hour = (int)(remainder / 3600);
    out->tm_mday = (int)days + 1;
    out->tm_mon = month;
    out->tm_year = year - 1900;
    out->tm_wday = (int)(((*seconds / 86400) + 4) % 7);
    out->tm_yday = (int)((*seconds / 86400) -
                         ((int64_t)(year - 1970) * 365));
    for (int current = 1970; current < year; ++current) {
        if (reliefos_tls_is_leap_year(current)) {
            --out->tm_yday;
        }
    }
    out->tm_isdst = 0;
    return out;
}

static int reliefos_tls_rdrand_available(void)
{
    uint32_t eax = 1;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    __asm__ volatile("cpuid"
                     : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "c"(0));
    (void)ebx;
    (void)edx;
    return (ecx & (1U << 30)) != 0;
}

static int reliefos_tls_rdrand64(uint64_t *value)
{
    unsigned char ready;
    __asm__ volatile("rdrand %0; setc %1"
                     : "=r"(*value), "=qm"(ready));
    return ready != 0;
}

int mbedtls_hardware_poll(void *data, unsigned char *output,
                          size_t len, size_t *olen)
{
    size_t written = 0;
    (void)data;
    if (olen) {
        *olen = 0;
    }
    if (!output || !olen || !reliefos_tls_rdrand_available()) {
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    }
    while (written < len) {
        uint64_t random = 0;
        int ready = 0;
        for (int attempt = 0; attempt < 10 && !ready; ++attempt) {
            ready = reliefos_tls_rdrand64(&random);
        }
        if (!ready) {
            return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
        }
        for (uint32_t index = 0; index < sizeof(random) && written < len; ++index) {
            output[written++] = (unsigned char)(random >> (index * 8U));
        }
    }
    *olen = written;
    return 0;
}

static int reliefos_tls_load_roots(void)
{
    struct reliefos_stat stat_info = {0};
    unsigned char *pem;
    uint32_t length = 0;
    int fd;
    int ret;
    if (reliefos_tls_roots_state != 0) {
        return reliefos_tls_roots_state > 0 ? 0 : -1;
    }
    reliefos_tls_roots_state = -1;
    ret = reliefos_stat_legacy(RELIEFOS_TLS_CA_BUNDLE, &stat_info);
    if (ret < 0 ||
        stat_info.type != RELIEFOS_FS_TYPE_FILE || stat_info.size == 0 ||
        stat_info.size > RELIEFOS_TLS_CA_BUNDLE_MAX) {
        printf("[tls] CA bundle invalid stat=%d type=%u size=%lu\n", ret,
               stat_info.type, (unsigned long)stat_info.size);
        return -1;
    }
    pem = malloc((size_t)stat_info.size + 1U);
    if (!pem) {
        printf("[tls] CA bundle allocation failed bytes=%lu\n",
               (unsigned long)stat_info.size);
        return -1;
    }
    fd = open(RELIEFOS_TLS_CA_BUNDLE, RELIEFOS_O_RDONLY, 0);
    if (fd < 0) {
        printf("[tls] CA bundle open failed ret=%d\n", fd);
        free(pem);
        return -1;
    }
    while ((uint64_t)length < stat_info.size) {
        long got = read(fd, pem + length, (size_t)(stat_info.size - length));
        if (got <= 0) {
            close(fd);
            printf("[tls] CA bundle read failed ret=%ld offset=%u\n", got, length);
            mbedtls_platform_zeroize(pem, (size_t)stat_info.size + 1U);
            free(pem);
            return -1;
        }
        length += (uint32_t)got;
    }
    close(fd);
    pem[length] = 0;
    mbedtls_x509_crt_init(&reliefos_tls_roots);
    ret = mbedtls_x509_crt_parse(&reliefos_tls_roots, pem, (size_t)length + 1U);
    mbedtls_platform_zeroize(pem, (size_t)length + 1U);
    free(pem);
    if (ret < 0 || reliefos_tls_roots.version == 0) {
        printf("[tls] CA bundle parse failed ret=%d roots=%u\n", ret,
               reliefos_tls_root_count());
        mbedtls_x509_crt_free(&reliefos_tls_roots);
        return -1;
    }
    printf("[tls] CA bundle loaded bytes=%u parse=%d roots=%u\n", length, ret,
           reliefos_tls_root_count());
    reliefos_tls_roots_state = 1;
    return 0;
}

static int reliefos_tls_send(void *context, const unsigned char *buffer,
                           size_t length)
{
    struct reliefos_tls_io *io = (struct reliefos_tls_io *)context;
    uint32_t status = RELIEFOS_NET_STATUS_TCP_FAILED;
    long sent = reliefos_socket_send(io->socket, buffer, (uint32_t)length,
                                   io->timeout_ms, &status);
    if (sent < 0 || status != RELIEFOS_NET_STATUS_OK) {
        printf("[tls] send failed socket=%d ret=%ld net=%u\n", io->socket,
               sent, status);
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return (int)sent;
}

static int reliefos_tls_recv(void *context, unsigned char *buffer,
                            size_t length)
{
    struct reliefos_tls_io *io = (struct reliefos_tls_io *)context;
    uint32_t status = RELIEFOS_NET_STATUS_TCP_FAILED;
    unsigned long started = reliefos_uptime_ms();
    for (;;) {
        uint32_t wait_ms = io->timeout_ms;
        long received;
        if (io->activity) {
            unsigned long elapsed = reliefos_uptime_ms() - started;
            if (elapsed >= io->timeout_ms ||
                io->activity(0, 0, io->activity_context) < 0) {
                printf("[tls] receive cancelled socket=%d elapsed=%lu\n",
                       io->socket, elapsed);
                return MBEDTLS_ERR_NET_RECV_FAILED;
            }
            wait_ms = io->timeout_ms - (uint32_t)elapsed;
            if (wait_ms > 200U) {
                wait_ms = 200U;
            }
        }
        received = reliefos_socket_recv(io->socket, buffer, (uint32_t)length,
                                      wait_ms, &status);
        if (received == 0 && status == RELIEFOS_NET_STATUS_TCP_TIMEOUT &&
            io->activity) {
            continue;
        }
        if (received == 0 && status == RELIEFOS_NET_STATUS_OK) {
            return 0;
        }
        if (received < 0 || status != RELIEFOS_NET_STATUS_OK) {
            printf("[tls] receive failed socket=%d ret=%ld net=%u\n",
                   io->socket, received, status);
            return MBEDTLS_ERR_NET_RECV_FAILED;
        }
        return (int)received;
    }
}

static int reliefos_tls_write_all(mbedtls_ssl_context *ssl,
                                 const unsigned char *data, uint32_t length)
{
    uint32_t written = 0;
    while (written < length) {
        int ret = mbedtls_ssl_write(ssl, data + written, length - written);
        if (ret <= 0) {
            return -1;
        }
        written += (uint32_t)ret;
    }
    return 0;
}

int reliefos_tls_http_exchange(int socket, const char *hostname,
                             uint32_t timeout_ms,
                             const void *request_headers,
                             uint32_t request_headers_len,
                             const void *request_body,
                             uint32_t request_body_len,
                             char *response, uint32_t response_capacity,
                             uint32_t *response_len)
{
    struct reliefos_time_info time_info;
    struct reliefos_tls_io io;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context entropy;
    uint32_t received = 0;
    uint32_t header_end = 0;
    uint32_t content_length = 0;
    uint32_t verify_flags;
    unsigned char read_buffer[4096];
    int roots_ret;
    int time_ret;
    int peer_tcp_eof = 0;
    int content_length_valid = 0;
    int response_complete = 0;
    int ret = -1;
    if (response_len) {
        *response_len = 0;
    }
    time_info = (struct reliefos_time_info){0};
    time_ret = reliefos_time_info(&time_info);
    roots_ret = reliefos_tls_load_roots();
    if (!hostname || !hostname[0] || !request_headers || !request_headers_len ||
        !response || response_capacity < 2U || !response_len || time_ret < 0 ||
        !time_info.valid || roots_ret < 0) {
        printf("[tls] exchange preflight host=%s time_ret=%d valid=%u roots_ret=%d\n",
               hostname ? hostname : "(null)", time_ret, time_info.valid, roots_ret);
        return -1;
    }
    printf("[tls] exchange host=%s clock=%u-%u-%u %u:%u:%u\n", hostname,
           time_info.year, time_info.month, time_info.day, time_info.hour,
           time_info.minute, time_info.second);
    io.socket = socket;
    io.timeout_ms = timeout_ms;
    io.activity = 0;
    io.activity_context = 0;
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&config);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_entropy_init(&entropy);
    ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                (const unsigned char *)"ReliefOS TLS", sizeof("ReliefOS TLS") - 1);
    if (ret != 0) {
        printf("[tls] exchange rng seed failed host=%s ret=%d\n", hostname, ret);
        goto cleanup;
    }
    ret = mbedtls_ssl_config_defaults(&config, MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        printf("[tls] exchange config failed host=%s ret=%d\n", hostname, ret);
        goto cleanup;
    }
    mbedtls_ssl_conf_min_tls_version(&config, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&config, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_authmode(&config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&config, &reliefos_tls_roots, 0);
    mbedtls_ssl_conf_rng(&config, mbedtls_ctr_drbg_random, &drbg);
    ret = mbedtls_ssl_setup(&ssl, &config);
    if (ret != 0) {
        printf("[tls] exchange setup failed host=%s ret=%d\n", hostname, ret);
        goto cleanup;
    }
    ret = mbedtls_ssl_set_hostname(&ssl, hostname);
    if (ret != 0) {
        printf("[tls] exchange hostname failed host=%s ret=%d\n", hostname, ret);
        goto cleanup;
    }
    mbedtls_ssl_set_bio(&ssl, &io, reliefos_tls_send, reliefos_tls_recv, 0);
    ret = mbedtls_ssl_handshake(&ssl);
    if (ret != 0) {
        printf("[tls] exchange handshake failed host=%s ret=%d\n", hostname, ret);
        goto cleanup;
    }
    verify_flags = mbedtls_ssl_get_verify_result(&ssl);
    if (verify_flags != 0) {
        reliefos_tls_log_verify_failure(hostname, verify_flags);
        goto cleanup;
    }
    printf("[tls] exchange verified host=%s\n", hostname);
    if (reliefos_tls_write_all(&ssl, request_headers, request_headers_len) < 0 ||
        (request_body_len &&
         reliefos_tls_write_all(&ssl, request_body, request_body_len) < 0)) {
        printf("[tls] exchange request write failed host=%s\n", hostname);
        goto cleanup;
    }
    while (received + 1U < response_capacity) {
        uint32_t read_capacity = response_capacity - received - 1U;
        if (read_capacity > sizeof(read_buffer)) {
            read_capacity = sizeof(read_buffer);
        }
        int got = mbedtls_ssl_read(&ssl, read_buffer, read_capacity);
        if (got == 0) {
            peer_tcp_eof = 1;
            response_complete = 1;
            break;
        }
        if (got == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            peer_tcp_eof = 1;
            response_complete = 1;
            break;
        }
        if (got < 0 && received == 0) {
            printf("[tls] exchange response failed host=%s ret=%d\n", hostname, got);
            goto cleanup;
        }
        if (got < 0) {
            goto cleanup;
        }
        memcpy(response + received, read_buffer, (size_t)got);
        received += (uint32_t)got;
        response[received] = 0;
        if (!header_end) {
            char *headers_end = strstr(response, "\r\n\r\n");
            if (headers_end) {
                header_end = (uint32_t)(headers_end - response) + 4U;
                content_length_valid = reliefos_tls_parse_content_length(
                    response, header_end, &content_length);
                if (content_length_valid) {
                    if (content_length <= UINT32_MAX - header_end &&
                        received >= header_end + content_length) {
                        response_complete = 1;
                        break;
                    }
                }
            }
        } else if (content_length_valid &&
                   content_length <= UINT32_MAX - header_end &&
                   received >= header_end + content_length) {
            response_complete = 1;
            break;
        }
    }
    if (!response_complete) {
        goto cleanup;
    }
    response[received] = 0;
    *response_len = received;
    ret = 0;

cleanup:
    if (!peer_tcp_eof) {
        mbedtls_ssl_close_notify(&ssl);
    }
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&config);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    return ret;
}

int reliefos_tls_http_stream(int socket, const char *hostname,
                           uint32_t timeout_ms,
                           const void *request_headers,
                           uint32_t request_headers_len,
                           const void *request_body,
                           uint32_t request_body_len,
                           reliefos_tls_stream_callback callback,
                           void *context)
{
    struct reliefos_time_info time_info;
    struct reliefos_tls_io io;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context entropy;
    unsigned char buffer[4096];
    uint32_t verify_flags;
    int roots_ret;
    int time_ret;
    int peer_tcp_eof = 0;
    int ret = -1;
    time_info = (struct reliefos_time_info){0};
    time_ret = reliefos_time_info(&time_info);
    roots_ret = reliefos_tls_load_roots();
    if (!hostname || !hostname[0] || !request_headers || !request_headers_len ||
        !callback || time_ret < 0 || !time_info.valid || roots_ret < 0) {
        printf("[tls] stream preflight host=%s time_ret=%d valid=%u roots_ret=%d\n",
               hostname ? hostname : "(null)", time_ret, time_info.valid, roots_ret);
        return -1;
    }
    printf("[tls] stream host=%s clock=%u-%u-%u %u:%u:%u\n", hostname,
           time_info.year, time_info.month, time_info.day, time_info.hour,
           time_info.minute, time_info.second);
    io.socket = socket;
    io.timeout_ms = timeout_ms;
    io.activity = callback;
    io.activity_context = context;
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&config);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_entropy_init(&entropy);
    ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                (const unsigned char *)"ReliefOS TLS", sizeof("ReliefOS TLS") - 1);
    if (ret != 0) {
        printf("[tls] stream rng seed failed host=%s ret=%d\n", hostname, ret);
        goto cleanup;
    }
    ret = mbedtls_ssl_config_defaults(&config, MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        printf("[tls] stream config failed host=%s ret=%d\n", hostname, ret);
        goto cleanup;
    }
    mbedtls_ssl_conf_min_tls_version(&config, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&config, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_authmode(&config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&config, &reliefos_tls_roots, 0);
    mbedtls_ssl_conf_rng(&config, mbedtls_ctr_drbg_random, &drbg);
    ret = mbedtls_ssl_setup(&ssl, &config);
    if (ret != 0) {
        printf("[tls] stream setup failed host=%s ret=%d\n", hostname, ret);
        goto cleanup;
    }
    ret = mbedtls_ssl_set_hostname(&ssl, hostname);
    if (ret != 0) {
        printf("[tls] stream hostname failed host=%s ret=%d\n", hostname, ret);
        goto cleanup;
    }
    mbedtls_ssl_set_bio(&ssl, &io, reliefos_tls_send, reliefos_tls_recv, 0);
    ret = mbedtls_ssl_handshake(&ssl);
    if (ret != 0) {
        printf("[tls] stream handshake failed host=%s ret=%d\n", hostname, ret);
        goto cleanup;
    }
    verify_flags = mbedtls_ssl_get_verify_result(&ssl);
    if (verify_flags != 0) {
        reliefos_tls_log_verify_failure(hostname, verify_flags);
        goto cleanup;
    }
    printf("[tls] stream verified host=%s\n", hostname);
    if (reliefos_tls_write_all(&ssl, request_headers, request_headers_len) < 0 ||
        (request_body_len &&
         reliefos_tls_write_all(&ssl, request_body, request_body_len) < 0)) {
        printf("[tls] stream request write failed host=%s\n", hostname);
        goto cleanup;
    }
    for (;;) {
        int got = mbedtls_ssl_read(&ssl, buffer, sizeof(buffer));
        if (got == 0) {
            peer_tcp_eof = 1;
            ret = 0;
            break;
        }
        if (got == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            peer_tcp_eof = 1;
            ret = 0;
            break;
        }
        if (got < 0) {
            printf("[tls] stream response failed host=%s ret=%d\n", hostname, got);
            break;
        }
        if (callback(buffer, (uint32_t)got, context) < 0) {
            break;
        }
    }

cleanup:
    if (!peer_tcp_eof) {
        mbedtls_ssl_close_notify(&ssl);
    }
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&config);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    return ret;
}
/* Published libleonos.so.2 aliases; keep these in the defining translation unit. */
extern __typeof__(reliefos_mbedtls_time) leonos_mbedtls_time __attribute__((alias("reliefos_mbedtls_time")));
extern __typeof__(reliefos_tls_http_exchange) leonos_tls_http_exchange __attribute__((alias("reliefos_tls_http_exchange")));
extern __typeof__(reliefos_tls_http_stream) leonos_tls_http_stream __attribute__((alias("reliefos_tls_http_stream")));
