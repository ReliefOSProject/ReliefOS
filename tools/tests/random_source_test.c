#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static bool available;
static unsigned calls, succeed_at, fail_from;
static bool random_hardware_available(void) { return available; }
static bool random_hardware_word(uint64_t *value)
{
    ++calls;
    *value = UINT64_C(0x8877665544332211);
    return calls >= succeed_at && (!fail_from || calls < fail_from);
}
#define RELIEFOS_RANDOM_TEST
#include "../../kernel/reliefnt/kernel/random/random.c"

int main(void)
{
    unsigned char bytes[19];
    memset(bytes, 0xa5, sizeof(bytes));
    assert(kernel_random_fill(NULL, 0) == 0);
    assert(kernel_random_fill(NULL, 1) == -14);
    assert(kernel_random_fill(bytes, 8) == -5 && calls == 0 && bytes[0] == 0xa5);
    available = true;
    succeed_at = 10;
    assert(kernel_random_fill(bytes, 9) == 0 && calls == 11);
    assert(bytes[0] == 0x11 && bytes[7] == 0x88 && bytes[8] == 0x11 && bytes[9] == 0xa5);
    calls = 0;
    succeed_at = 11;
    assert(kernel_random_fill(bytes, 8) == -5 && calls == 10);
    calls = 0;
    succeed_at = 1;
    fail_from = 2;
    assert(kernel_random_fill(bytes, 19) == -5 && calls == 11);
    for (unsigned i = 0; i < 8; ++i) assert(bytes[i] == 0);
    return 0;
}
