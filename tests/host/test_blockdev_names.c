/* Exercise the real GPT name codec without opening any device. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include "../../userland/runtime/src/blockdev.c"

int main(void)
{
    uint16_t name[36];
    char result[144];
    const char *cases[] = {"", "ReliefOS Data", "中文资料盘", "文\xF0\x9F\x92\xBE"};
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        assert(block_name_set(name, cases[i]) == 0);
        block_name_get(result, sizeof(result), name);
        assert(!strcmp(result, cases[i]));
    }
    const char *bad[] = {"\xC0\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xE4\xB8"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        assert(block_name_set(name, bad[i]) == -EILSEQ);
    assert(block_name_set(name, "1234567890123456789012345678901234567") == -ENAMETOOLONG);
    assert(block_name_set(name, "123456789012345678901234567890123456") == 0);
    block_name_get(result, sizeof(result), name);
    assert(strlen(result) == 36);
    assert(block_name_set(name, "中文") == 0);
    block_name_get(result, 5, name);
    assert(!strcmp(result, "中")); // Never split a UTF-8 character.
    name[0] = 0xD800; name[1] = 'A'; name[2] = 0;
    block_name_get(result, sizeof(result), name);
    assert(!strcmp(result, "\xEF\xBF\xBD" "A"));
    puts("PASS GPT names: Chinese/non-BMP roundtrip, malformed UTF-8, UTF-16 capacity and bounded output");
    return 0;
}
