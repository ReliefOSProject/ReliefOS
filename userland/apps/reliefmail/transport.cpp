#include "transport.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace rmail {
namespace {

constexpr size_t MaxCaBundleBytes = 4u * 1024u * 1024u;
constexpr size_t MaxPeerCertificateDetails = 768;
constexpr const char *SystemCaBundle = "/etc/ssl/certs/ca-certificates.crt";

struct TransportState {
    int socket_fd;
    uint32_t port;
    bool socket_open;
    bool tls_ready;
    bool secure;
    int callback_errno;
    char *hostname;
    char *ca_path;
    char peer_certificate_details[MaxPeerCertificateDetails];
    size_t peer_certificate_details_size;
    bool peer_certificate_details_captured;
    bool peer_certificate_details_overflow;
    const OperationContext *operation;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context entropy;
    mbedtls_x509_crt ca;
};

int capture_peer_certificate(void *context, mbedtls_x509_crt *certificate,
                             int depth, uint32_t *verification_flags) {
    /* Keep mbedTLS's verification result authoritative: this callback only
     * snapshots public leaf-certificate details and never changes flags. */
    (void) verification_flags;
    if (depth != 0) return 0;
    TransportState *state = static_cast<TransportState *>(context);
    if (!state || !certificate) return 0;
    memset(state->peer_certificate_details, 0, sizeof(state->peer_certificate_details));
    state->peer_certificate_details_size = 0;
    state->peer_certificate_details_captured = false;
    state->peer_certificate_details_overflow = false;
    const int details = mbedtls_x509_crt_info(state->peer_certificate_details,
        sizeof(state->peer_certificate_details), "", certificate);
    if (details < 0 || static_cast<size_t>(details) >= sizeof(state->peer_certificate_details)) {
        state->peer_certificate_details_overflow = true;
        memset(state->peer_certificate_details, 0, sizeof(state->peer_certificate_details));
        return 0;
    }
    state->peer_certificate_details_size = static_cast<size_t>(details);
    state->peer_certificate_details[details] = 0;
    state->peer_certificate_details_captured = details > 0;
    return 0;
}

uint64_t monotonic_milliseconds() {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0) return UINT64_MAX;
    const uint64_t seconds = static_cast<uint64_t>(now.tv_sec);
    if (seconds > (UINT64_MAX - static_cast<uint64_t>(now.tv_nsec / 1000000)) / 1000)
        return UINT64_MAX;
    return seconds * 1000 + static_cast<uint64_t>(now.tv_nsec / 1000000);
}

bool check_operation(const OperationContext &operation, Text &error) {
    if (operation.is_cancelled && operation.is_cancelled(operation.cancellation_context)) {
        error.set("network operation cancelled");
        return false;
    }
    if (operation.deadline_monotonic_ms != UINT64_MAX) {
        const uint64_t now = monotonic_milliseconds();
        if (now == UINT64_MAX || now >= operation.deadline_monotonic_ms) {
            error.set("network operation deadline expired");
            return false;
        }
    }
    return true;
}

bool wait_socket(int socket_fd, short events, const OperationContext &operation, Text &error) {
    for (;;) {
        if (!check_operation(operation, error)) return false;
        int timeout = -1;
        if (operation.deadline_monotonic_ms != UINT64_MAX) {
            const uint64_t now = monotonic_milliseconds();
            if (now == UINT64_MAX || now >= operation.deadline_monotonic_ms) {
                error.set("network operation deadline expired");
                return false;
            }
            const uint64_t remaining = operation.deadline_monotonic_ms - now;
            timeout = remaining > static_cast<uint64_t>(INT_MAX) ? INT_MAX : static_cast<int>(remaining);
        }
        struct pollfd descriptor;
        descriptor.fd = socket_fd;
        descriptor.events = events;
        descriptor.revents = 0;
        const int result = poll(&descriptor, 1, timeout);
        if (result > 0) {
            if (descriptor.revents & events) return true;
            if (descriptor.revents & POLLHUP) {
                if (events & POLLIN) return true;
                error.set("network peer closed the connection");
                return false;
            }
            error.set("network socket failed");
            return false;
        }
        if (result == 0) {
            error.set("network operation deadline expired");
            return false;
        }
        if (errno != EINTR) {
            error.set("network poll failed");
            return false;
        }
    }
}

int set_nonblocking(int socket_fd) {
    const int flags = fcntl(socket_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    return 0;
}

bool connect_socket(TransportState &state, const char *host, uint32_t port,
                    const OperationContext &operation, Text &error) {
    if (!host || !*host || port == 0 || port > 65535) {
        error.set("invalid network endpoint");
        return false;
    }
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    struct addrinfo *addresses = nullptr;
    char port_text[6];
    const int port_length = snprintf(port_text, sizeof(port_text), "%u", port);
    if (port_length <= 0 || static_cast<size_t>(port_length) >= sizeof(port_text)) {
        error.set("invalid network port");
        return false;
    }
    /* getaddrinfo is synchronous; the worker watchdog bounds this resolver call. */
    if (!check_operation(operation, error)) return false;
    const int lookup = getaddrinfo(host, port_text, &hints, &addresses);
    if (lookup != 0) {
        error.set("server name could not be resolved");
        return false;
    }
    bool connected = false;
    for (struct addrinfo *address = addresses; address; address = address->ai_next) {
        if (!check_operation(operation, error)) break;
        const int fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) continue;
        if (set_nonblocking(fd) != 0) {
            close(fd);
            continue;
        }
        state.socket_fd = fd;
        state.socket_open = true;
        const int result = ::connect(fd, address->ai_addr, address->ai_addrlen);
        if (result == 0) { connected = true; break; }
        if (errno != EINPROGRESS && errno != EWOULDBLOCK) {
            close(fd);
            state.socket_fd = -1;
            state.socket_open = false;
            continue;
        }
        if (!wait_socket(fd, POLLOUT, operation, error)) {
            close(fd);
            state.socket_fd = -1;
            state.socket_open = false;
            if (error == "network operation deadline expired" || error == "network operation cancelled")
                break;
            continue;
        }
        int socket_error = 0;
        socklen_t socket_error_size = sizeof(socket_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size) == 0 && socket_error == 0) {
            connected = true;
            break;
        }
        close(fd);
        state.socket_fd = -1;
        state.socket_open = false;
    }
    freeaddrinfo(addresses);
    if (connected) error.clear();
    if (!connected && error.empty()) error.set("could not connect to mail server");
    return connected;
}

bool read_ca_bundle(mbedtls_x509_crt &chain, const char *path, Text &error) {
    const char *selected_path = path && *path ? path : SystemCaBundle;
    const int fd = open(selected_path, O_RDONLY);
    if (fd < 0) {
        error.set("could not open the configured CA certificate file");
        return false;
    }
    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
        static_cast<uint64_t>(info.st_size) > MaxCaBundleBytes) {
        close(fd);
        error.set("CA certificate file is invalid or exceeds the size limit");
        return false;
    }
    const size_t length = static_cast<size_t>(info.st_size);
    unsigned char *bytes = static_cast<unsigned char *>(malloc(length + 1));
    if (!bytes) {
        close(fd);
        error.set("out of memory while reading CA certificates");
        return false;
    }
    size_t received = 0;
    while (received < length) {
        const ssize_t count = read(fd, bytes + received, length - received);
        if (count > 0) { received += static_cast<size_t>(count); continue; }
        if (count < 0 && errno == EINTR) continue;
        free(bytes);
        close(fd);
        error.set("could not read CA certificate file");
        return false;
    }
    close(fd);
    bytes[length] = 0;
    const int parsed = mbedtls_x509_crt_parse(&chain, bytes, length + 1);
    memset(bytes, 0, length + 1);
    free(bytes);
    if (parsed != 0 || !chain.raw.p) {
        error.set("CA certificate file could not be parsed");
        return false;
    }
    return true;
}

void release_tls(TransportState &state) {
    if (state.tls_ready) {
        mbedtls_ssl_free(&state.ssl);
        mbedtls_ssl_config_free(&state.config);
        mbedtls_ctr_drbg_free(&state.drbg);
        mbedtls_entropy_free(&state.entropy);
        mbedtls_x509_crt_free(&state.ca);
        state.tls_ready = false;
        state.secure = false;
    }
}

void close_state(TransportState &state) {
    release_tls(state);
    if (state.socket_open) close(state.socket_fd);
    state.socket_fd = -1;
    state.socket_open = false;
    if (state.hostname) free(state.hostname);
    state.hostname = nullptr;
    if (state.ca_path) free(state.ca_path);
    state.ca_path = nullptr;
}

int tls_send(void *context, const unsigned char *buffer, size_t length) {
    TransportState *state = static_cast<TransportState *>(context);
    const size_t bounded = length > 65536 ? 65536 : length;
    const ssize_t result = send(state->socket_fd, buffer, bounded, MSG_NOSIGNAL);
    if (result >= 0) return static_cast<int>(result);
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_WRITE;
    state->callback_errno = errno;
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

int tls_receive(void *context, unsigned char *buffer, size_t length) {
    TransportState *state = static_cast<TransportState *>(context);
    const size_t bounded = length > 65536 ? 65536 : length;
    const ssize_t result = recv(state->socket_fd, buffer, bounded, 0);
    if (result >= 0) return static_cast<int>(result);
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_READ;
    state->callback_errno = errno;
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

bool begin_tls(TransportState &state, const char *ca_path,
               const OperationContext &operation, Text &error) {
    if (!state.socket_open || state.secure) {
        error.set("TLS cannot be started on this connection");
        return false;
    }
    mbedtls_ssl_init(&state.ssl);
    mbedtls_ssl_config_init(&state.config);
    mbedtls_ctr_drbg_init(&state.drbg);
    mbedtls_entropy_init(&state.entropy);
    mbedtls_x509_crt_init(&state.ca);
    state.tls_ready = true;
    if (!read_ca_bundle(state.ca, ca_path, error)) { release_tls(state); return false; }
    int result = mbedtls_ctr_drbg_seed(&state.drbg, mbedtls_entropy_func, &state.entropy,
        reinterpret_cast<const unsigned char *>("ReliefMail TLS"), sizeof("ReliefMail TLS") - 1);
    if (result != 0) { error.set("TLS random generator setup failed"); release_tls(state); return false; }
    result = mbedtls_ssl_config_defaults(&state.config, MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (result != 0) { error.set("TLS configuration failed"); release_tls(state); return false; }
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    mbedtls_ssl_conf_min_version(&state.config, MBEDTLS_SSL_MAJOR_VERSION_3,
        MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_max_version(&state.config, MBEDTLS_SSL_MAJOR_VERSION_3,
        MBEDTLS_SSL_MINOR_VERSION_3);
#pragma GCC diagnostic pop
    mbedtls_ssl_conf_authmode(&state.config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&state.config, &state.ca, nullptr);
    mbedtls_ssl_conf_verify(&state.config, capture_peer_certificate, &state);
    mbedtls_ssl_conf_rng(&state.config, mbedtls_ctr_drbg_random, &state.drbg);
    result = mbedtls_ssl_setup(&state.ssl, &state.config);
    if (result != 0) { error.set("TLS session setup failed"); release_tls(state); return false; }
    result = mbedtls_ssl_set_hostname(&state.ssl, state.hostname);
    if (result != 0) { error.set("TLS server identity setup failed"); release_tls(state); return false; }
    mbedtls_ssl_set_bio(&state.ssl, &state, tls_send, tls_receive, nullptr);
    state.operation = &operation;
    for (;;) {
        if (!check_operation(operation, error)) { state.operation = nullptr; release_tls(state); return false; }
        result = mbedtls_ssl_handshake(&state.ssl);
        if (result == 0) break;
        if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) {
            const short events = result == MBEDTLS_ERR_SSL_WANT_READ ? POLLIN : POLLOUT;
            if (wait_socket(state.socket_fd, events, operation, error)) continue;
            state.operation = nullptr;
            release_tls(state);
            return false;
        }
        if (mbedtls_ssl_get_verify_result(&state.ssl) != 0)
            error.set("TLS certificate or server identity verification failed");
        else error.set("TLS handshake failed");
        state.operation = nullptr;
        release_tls(state);
        return false;
    }
    state.operation = nullptr;
    if (mbedtls_ssl_get_verify_result(&state.ssl) != 0) {
        error.set("TLS certificate or server identity verification failed");
        release_tls(state);
        return false;
    }
    state.secure = true;
    return true;
}

TransportState *make_state(Text &error) {
    TransportState *state = static_cast<TransportState *>(calloc(1, sizeof(TransportState)));
    if (!state) error.set("out of memory while creating network stream");
    else state->socket_fd = -1;
    return state;
}

} // namespace

Stream::Stream() = default;

Stream::~Stream() { close(); }

bool Stream::connect(const char *host, uint32_t port, TransportSecurity tls_mode,
                     const char *ca_path, const OperationContext &context, Text &error) {
    close();
    error.clear();
    if (!host || !*host || strlen(host) > 255 || port == 0 || port > 65535) {
        error.set("invalid network endpoint");
        return false;
    }
    TransportState *state = make_state(error);
    if (!state) return false;
    state->hostname = static_cast<char *>(malloc(strlen(host) + 1));
    const char *selected_ca_path = ca_path && *ca_path ? ca_path : nullptr;
    state->ca_path = selected_ca_path ? static_cast<char *>(malloc(strlen(selected_ca_path) + 1)) : nullptr;
    if (!state->hostname || (selected_ca_path && !state->ca_path)) {
        if (state->hostname) free(state->hostname);
        if (state->ca_path) free(state->ca_path);
        free(state);
        error.set("out of memory while preparing server verification");
        return false;
    }
    memcpy(state->hostname, host, strlen(host) + 1);
    if (selected_ca_path) memcpy(state->ca_path, selected_ca_path, strlen(selected_ca_path) + 1);
    state->port = port;
    implementation_ = state;
    if (!connect_socket(*state, host, port, context, error)) { close(); return false; }
    if (tls_mode == TransportSecurity::ImplicitTls && !begin_tls(*state, ca_path, context, error)) {
        close();
        return false;
    }
    error.clear();
    return true;
}

bool Stream::read(unsigned char *buffer, size_t capacity, size_t *read_size,
                  const OperationContext &context, Text &error) {
    error.clear();
    if (read_size) *read_size = 0;
    TransportState *state = static_cast<TransportState *>(implementation_);
    if (!state || !state->socket_open || (!buffer && capacity) || !read_size) {
        error.set("network stream is not readable");
        return false;
    }
    if (!capacity) return true;
    const size_t bounded = capacity > 16384 ? 16384 : capacity;
    for (;;) {
        if (!check_operation(context, error)) return false;
        if (state->secure) {
            const int result = mbedtls_ssl_read(&state->ssl, buffer, bounded);
            if (result > 0) { *read_size = static_cast<size_t>(result); return true; }
            if (result == 0 || result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                error.set("network peer closed the connection");
                return false;
            }
            if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) {
                if (!wait_socket(state->socket_fd, result == MBEDTLS_ERR_SSL_WANT_READ ? POLLIN : POLLOUT,
                    context, error)) return false;
                continue;
            }
            if (mbedtls_ssl_get_verify_result(&state->ssl) != 0)
                error.set("TLS certificate verification failed");
            else error.set("TLS read failed");
            return false;
        }
        const ssize_t result = recv(state->socket_fd, buffer, bounded, 0);
        if (result > 0) { *read_size = static_cast<size_t>(result); return true; }
        if (result == 0) { error.set("network peer closed the connection"); return false; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (!wait_socket(state->socket_fd, POLLIN, context, error)) return false;
            continue;
        }
        error.set("network read failed");
        return false;
    }
}

bool Stream::write(const unsigned char *buffer, size_t size, size_t *written_size,
                   const OperationContext &context, Text &error) {
    error.clear();
    if (written_size) *written_size = 0;
    TransportState *state = static_cast<TransportState *>(implementation_);
    if (!state || !state->socket_open || (!buffer && size) || !written_size) {
        error.set("network stream is not writable");
        return false;
    }
    if (!size) return true;
    const size_t bounded = size > 16384 ? 16384 : size;
    for (;;) {
        if (!check_operation(context, error)) return false;
        if (state->secure) {
            const int result = mbedtls_ssl_write(&state->ssl, buffer, bounded);
            if (result > 0) { *written_size = static_cast<size_t>(result); return true; }
            if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) {
                if (!wait_socket(state->socket_fd, result == MBEDTLS_ERR_SSL_WANT_READ ? POLLIN : POLLOUT,
                    context, error)) return false;
                continue;
            }
            error.set("TLS write failed");
            return false;
        }
        const ssize_t result = send(state->socket_fd, buffer, bounded, MSG_NOSIGNAL);
        if (result > 0) { *written_size = static_cast<size_t>(result); return true; }
        if (result == 0) { error.set("network write made no progress"); return false; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (!wait_socket(state->socket_fd, POLLOUT, context, error)) return false;
            continue;
        }
        error.set("network write failed");
        return false;
    }
}

bool Stream::start_tls(const OperationContext &context, Text &error) {
    error.clear();
    TransportState *state = static_cast<TransportState *>(implementation_);
    if (!state || !state->socket_open || state->secure) {
        error.set("TLS cannot be started on this connection");
        return false;
    }
    if (!begin_tls(*state, state->ca_path, context, error)) {
        close();
        return false;
    }
    return true;
}

bool Stream::diagnostics(TransportDiagnostics &snapshot) const {
    snapshot = TransportDiagnostics();
    const TransportState *state = static_cast<const TransportState *>(implementation_);
    if (!state || !state->socket_open || !state->secure || !state->tls_ready) return false;
    const char *version = mbedtls_ssl_get_version(&state->ssl);
    const char *cipher = mbedtls_ssl_get_ciphersuite(&state->ssl);
    if (!version || !cipher || !state->hostname || !state->peer_certificate_details_captured ||
        state->peer_certificate_details_overflow) return false;
    snapshot.tls_active = true;
    snapshot.certificate_verified = true;
    snapshot.certificate_verify_flags = mbedtls_ssl_get_verify_result(&state->ssl);
    snapshot.port = state->port;
    return snapshot.server_name.set(state->hostname) && snapshot.tls_version.set(version) &&
        snapshot.cipher_suite.set(cipher) &&
        snapshot.peer_certificate_details.set(state->peer_certificate_details,
            state->peer_certificate_details_size) &&
        snapshot.certificate_verify_flags == 0;
}

void Stream::close() {
    TransportState *state = static_cast<TransportState *>(implementation_);
    implementation_ = nullptr;
    if (!state) return;
    close_state(*state);
    memset(state, 0, sizeof(*state));
    free(state);
}

bool Stream::connected() const {
    const TransportState *state = static_cast<const TransportState *>(implementation_);
    return state && state->socket_open;
}

bool Stream::secure() const {
    const TransportState *state = static_cast<const TransportState *>(implementation_);
    return state && state->secure;
}

} // namespace rmail
