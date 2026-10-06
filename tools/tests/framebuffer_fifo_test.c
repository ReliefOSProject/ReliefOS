#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include "framebuffer_fifo.h"

static void test_empty_and_exact_tail(void)
{
    uint32_t at, after;
    assert(framebuffer_fifo_reserve(16, 64, 16, 16, 20, &at, &after));
    assert(at == 16 && after == 36);
    assert(framebuffer_fifo_reserve(16, 64, 44, 24, 20, &at, &after));
    assert(at == 44 && after == 16);
}

static void test_wrap_and_full(void)
{
    uint32_t at, after;
    assert(framebuffer_fifo_reserve(16, 64, 56, 40, 20, &at, &after));
    assert(at == 16 && after == 36);
    assert(!framebuffer_fifo_reserve(16, 64, 40, 60, 20, &at, &after));
    assert(!framebuffer_fifo_reserve(16, 36, 16, 16, 20, &at, &after));
}

int main(void)
{
    test_empty_and_exact_tail();
    test_wrap_and_full();
    return 0;
}
