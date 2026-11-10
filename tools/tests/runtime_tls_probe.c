/* Target-only TLS verification probe; sends no credentials or private data. */
#include <reliefos/net.h>
#include <reliefos/tls.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc != 5) return 2;
    int fd = reliefos_socket_tcp();
    struct reliefos_net_socket_connect connection = {0};
    if (fd < 0 || reliefos_socket_connect(fd, argv[1], (uint32_t)atoi(argv[3]), 15000, &connection) < 0) {
        printf("TLS_PROBE_CONNECT_FAILED host=%s\n", argv[1]);
        if (fd >= 0) close(fd);
        return 2;
    }
    const char request[] = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    char response[16384]; uint32_t length = 0;
    int result = reliefos_tls_http_exchange(fd, argv[2], 15000, request, sizeof(request)-1, NULL, 0, response, sizeof(response), &length);
    close(fd);
    const int accepted = result == 0 && length > 0;
    const int expected = !strcmp(argv[4], "accept");
    printf("TLS_PROBE host=%s expected=%s accepted=%d result=%d bytes=%u\n", argv[2], argv[4], accepted, result, length);
    return accepted != expected;
}
