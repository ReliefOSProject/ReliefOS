/* Target/host diagnostic probe: completes TLS verification without logging in
 * or sending mail. Linked with the production ReliefMail transport. */
#include "transport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) return 2;
    char *end = nullptr;
    const unsigned long port = strtoul(argv[2], &end, 10);
    if (!end || *end || port == 0 || port > 65535) return 2;
    timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 2;
    rmail::OperationContext context;
    context.deadline_monotonic_ms = static_cast<uint64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000 + 15000;
    rmail::Stream stream;
    rmail::Text error;
    if (!stream.connect(argv[1], static_cast<uint32_t>(port), rmail::TransportSecurity::ImplicitTls,
            argc == 4 ? argv[3] : nullptr, context, error)) {
        printf("MAIL_TLS_PROBE_FAILED host=%s problem=%u error=%s\n", argv[1],
            static_cast<unsigned int>(rmail::transport_problem(error)), error.c_str());
        return 1;
    }
    if (!stream.secure()) return 1;
    printf("MAIL_TLS_PROBE_OK host=%s port=%lu verified=1 authentication_sent=0\n", argv[1], port);
    stream.close();
    return 0;
}
