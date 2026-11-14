/* Exercise actual libmd/libbsd APIs with their exported development headers. */
#include <bsd/string.h>
#include <md5.h>
#include <sha256.h>
#include <sha512.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int digest_matches(const char *name, const char *actual, const char *expected)
{
    if (actual && strcmp(actual, expected) == 0)
        return 0;
    fprintf(stderr, "%s: got %s, expected %s\n", name, actual ? actual : "NULL", expected);
    return 1;
}

int main(int argc, char **argv)
{
    static const unsigned char input[] = "abc";
    static const char md5[] = "900150983cd24fb0d6963f7d28e17f72";
    char buffer[129], permissions[12];
    int failures = 0;

    if (argc == 3 && strcmp(argv[1], "--deny") == 0) {
        errno = 0;
        if (MD5File(argv[2], buffer) != NULL || errno != EACCES) {
            fprintf(stderr, "MD5File denied read: errno=%d\n", errno);
            return 1;
        }
        puts("PASS libmd respects file read permissions");
        return 0;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: %s FILE_CONTAINING_abc | --deny PROTECTED_FILE\n", argv[0]);
        return 2;
    }
    failures += digest_matches("MD5Data", MD5Data(input, 3, buffer), md5);
    failures += digest_matches("SHA256Data", SHA256Data(input, 3, buffer),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    failures += digest_matches("SHA512Data", SHA512Data(input, 3, buffer),
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
        "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    failures += digest_matches("MD5File", MD5File(argv[1], buffer), md5);
    errno = 0;
    if (MD5File("/no-such-libmd-runtime-probe", buffer) != NULL || errno != ENOENT) {
        fprintf(stderr, "MD5File missing input: errno=%d\n", errno);
        failures++;
    }
    strmode(S_IFREG | 0644, permissions);
    failures += digest_matches("libbsd strmode", permissions, "-rw-r--r-- ");
    if (failures)
        return 1;
    puts("PASS official libmd digest vectors, file errors and libbsd dependency");
    return 0;
}
