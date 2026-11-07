#ifndef RELIEFMAIL_TRANSPORT_H
#define RELIEFMAIL_TRANSPORT_H

#include "model.h"

namespace rmail {

enum class TransportProblem : uint8_t {
    Other, CaUnavailable, CaInvalid, UntrustedCertificate, CertificateTime,
    HostnameMismatch, Handshake
};

/* Stable diagnostic tags survive worker IPC; descriptive details contain no
 * authentication data. They must never be used to relax verification. */
TransportProblem transport_problem(const Text &diagnostic);

/* Absolute CLOCK_MONOTONIC deadline in milliseconds; UINT64_MAX means none. */
using CancellationCheck = bool (*)(void *context);

struct OperationContext {
    uint64_t deadline_monotonic_ms = UINT64_MAX;
    CancellationCheck is_cancelled = nullptr;
    void *cancellation_context = nullptr;
};

/* Socket/TLS waits observe this deadline and cancellation callback. POSIX
 * getaddrinfo is synchronous on the target and cannot be interrupted here;
 * the Task5 worker watchdog bounds that resolver interval. */

/* A non-owning view. Callers keep secrets in wipeable storage and the protocol
 * session must not retain or log this view. */
struct SecretView {
    const unsigned char *data = nullptr;
    size_t size = 0;
};

/* Immutable, offset-readable message bytes, normally backed by a completed
 * private spool. A source is bounded by MaxRawMessageBytes. */
using MessageReadCallback = bool (*)(const void *context, size_t offset,
    unsigned char *buffer, size_t capacity, size_t *read_size);

struct MessageSource {
    const void *context = nullptr;
    MessageReadCallback read = nullptr;
    size_t size = 0;
};

/* Read-only bounded diagnostics. Certificate details are public peer data;
 * no credentials, protocol payloads, or authentication replies are exposed. */
struct TransportDiagnostics {
    bool tls_active = false;
    bool certificate_verified = false;
    uint32_t certificate_verify_flags = 0;
    uint32_t port = 0;
    Text server_name;
    Text tls_version;
    Text cipher_suite;
    Text peer_certificate_details;
};

class Stream {
    void *implementation_ = nullptr;
    Stream(const Stream &) = delete;
    Stream &operator=(const Stream &) = delete;

public:
    Stream();
    ~Stream();

    bool connect(const char *host, uint32_t port, TransportSecurity tls_mode,
        const char *ca_path, const OperationContext &context, Text &error);
    bool read(unsigned char *buffer, size_t capacity, size_t *read_size,
        const OperationContext &context, Text &error);
    bool write(const unsigned char *buffer, size_t size, size_t *written_size,
        const OperationContext &context, Text &error);
    bool start_tls(const OperationContext &context, Text &error);
    bool diagnostics(TransportDiagnostics &snapshot) const;
    void close();
    bool connected() const;
    bool secure() const;
};

}

#endif
