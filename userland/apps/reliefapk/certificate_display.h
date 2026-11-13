#ifndef RELIEFAPK_CERTIFICATE_DISPLAY_H
#define RELIEFAPK_CERTIFICATE_DISPLAY_H

#include <libintl.h>
#include <stddef.h>
#include <string.h>

namespace ra {
/* This is a presentation filter only: the Process buffers and its protocol
 * parser always retain their original bytes. Unknown lines pass through. */
class CertificateDisplay {
    static constexpr size_t Limit = 4096;
    char pending_[Limit + 1];
    size_t used_ = 0;
    bool passthrough_ = false;

    static const char *known(const char *value, size_t size) {
#define N_(s) (s)
        static const char *const keys[] = {
            N_("Root authentication required for certificate repair"),
            N_("Root authentication required"),
            N_("Invalid privileged apk request"),
            N_("A trusted OS recovery CA bundle is unavailable. Update the ReliefOS trust package or image."),
            N_("CA RECOVERY PLAN: the system CA file is unusable. Apply will first restore the trusted OS CA bundle, preserving the old regular file in a private backup."),
            N_("The two CA world requests will be refreshed as plain package names (without version or tag constraints); all other world requests are preserved."),
            N_("Online package simulation is deferred until trust is restored. The authenticated worker will then check signed repositories, simulate and repair ca-certificates and ca-certificates-bundle together."),
            N_("Could not determine whether the system CA package is installed"),
            N_("Repair targets: ca-certificates and ca-certificates-bundle (package signatures and TLS verification required)."),
            N_("Refresh the two world requests as plain package names without version/tag constraints, then reinstall both so source certificates and generated output agree. Other world requests are preserved."),
            N_("Verified CA package metadata is unavailable. Check HTTPS, repository signatures, keys and sources. The restored OS CA bundle, if any, has been retained."),
            N_("APK left an updated CA bundle as .apk-new. Check the protected-path policy before treating this as an updated trust store."),
            N_("The installed CA bundle is still unusable. Check the APK protected-path policy and .apk-new files; update the ReliefOS trust package/configuration."),
            N_("System CA bundle installed and parsed successfully"),
            N_("APK repository diagnostic redacted; check configured sources and TLS."),
            N_("APK skipped an unavailable package or its output was incomplete; certificate repair failed."),
            N_("could not prepare the certificate package operation"),
            N_("CA parent directories must be root-owned and not group/other writable or symlinks"),
            N_("CA directory ownership or permissions are unsafe"),
            N_("Cannot inspect the current CA trust path"),
            N_("CA trust file must be an owned regular file, not group/other writable, and at most 4 MiB"),
            N_("CA recovery directory ownership or permissions are unsafe"),
            N_("OS recovery CA bundle is empty or too large"),
            N_("OS recovery CA bundle has no usable certificates"),
            N_("Cannot inspect the current CA bundle"),
            N_("Existing CA bundle must be an owned regular file of at most 4 MiB"),
            N_("Could not prepare the complete OS recovery CA bundle"),
            N_("Could not preserve the previous CA bundle"),
            N_("Could not preserve the complete previous CA bundle"),
            N_("Could not sync the previous CA bundle backup"),
            N_("Could not sync CA recovery files"),
            N_("CA bundle changed during recovery; retry after checking the system state"),
            N_("Could not atomically install the OS recovery CA bundle"),
            N_("OS CA bundle was restored, but directory synchronization failed"),
            N_("none (file was missing)"),
        };
#undef N_
        for (const char *key : keys)
            if (strlen(key) == size && !memcmp(value, key, size))
                return gettext(key);
        return nullptr;
    }

    using Emit = void (*)(void *, const char *, size_t);
    static void emit_line(const char *line, size_t size, Emit emit, void *context) {
#define N_(s) (s)
        static const char *const prefixes[] = {
            N_("Unsafe system CA trust path; no online operation started: "),
            N_("OS CA recovery failed: "),
            N_("OS CA baseline restored atomically; previous file backup: "),
        };
#undef N_
        if (const char *translated = known(line, size)) {
            emit(context, translated, strlen(translated));
            return;
        }
        for (const char *prefix : prefixes) {
            const size_t length = strlen(prefix);
            if (size < length || memcmp(line, prefix, length)) continue;
            const char *translated = gettext(prefix);
            emit(context, translated, strlen(translated));
            const char *tail = line + length;
            const size_t tail_size = size - length;
            if (const char *detail = known(tail, tail_size))
                emit(context, detail, strlen(detail));
            else
                emit(context, tail, tail_size);
            return;
        }
        emit(context, line, size);
    }

public:
    void reset() { used_ = 0; passthrough_ = false; }
    void feed(const char *bytes, size_t size, Emit emit, void *context) {
        for (size_t at = 0; at < size;) {
            if (passthrough_) {
                const char *newline = static_cast<const char *>(memchr(bytes + at, '\n', size - at));
                size_t count = newline ? static_cast<size_t>(newline - (bytes + at)) + 1 : size - at;
                emit(context, bytes + at, count);
                at += count;
                if (newline) passthrough_ = false;
            } else if (bytes[at] == '\n') {
                pending_[used_] = 0;
                emit_line(pending_, used_, emit, context);
                emit(context, "\n", 1);
                used_ = 0;
                ++at;
            } else if (used_ < Limit) {
                pending_[used_++] = bytes[at++];
            } else {
                pending_[used_] = 0;
                emit(context, pending_, used_);
                used_ = 0;
                passthrough_ = true;
            }
        }
    }
    void flush(Emit emit, void *context) {
        if (used_) {
            pending_[used_] = 0;
            emit(context, pending_, used_);
        }
        reset();
    }
};
}
#endif
