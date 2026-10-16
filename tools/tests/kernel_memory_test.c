#define _GNU_SOURCE

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

void *kernel_memcpy(void *dst, const void *src, size_t len);
void *kernel_memset(void *dst, int value, size_t len);

enum {
    SMALL_CAPACITY = 640,
    RANDOM_CAPACITY = 4352,
    RANDOM_MAX_LENGTH = 4096,
};

typedef struct {
    uint8_t *mapping;
    uint8_t *data;
    size_t page_size;
    size_t data_size;
    size_t mapping_size;
} GuardedRegion;

static unsigned int random_state = 0x9e3779b9U;

static void fail(const char *message)
{
    fprintf(stderr, "kernel memory test failed: %s\n", message);
    exit(EXIT_FAILURE);
}

static void fail_at(const char *operation, size_t case_number, size_t offset,
                    uint8_t expected, uint8_t actual)
{
    fprintf(stderr,
            "kernel memory test failed: %s case=%zu offset=%zu "
            "expected=0x%02x actual=0x%02x\n",
            operation, case_number, offset, (unsigned int)expected,
            (unsigned int)actual);
    exit(EXIT_FAILURE);
}

static uint32_t next_random(void)
{
    uint32_t value = random_state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    random_state = value;
    return value;
}

static uint8_t pattern_byte(size_t index, unsigned int salt)
{
    uint32_t value = (uint32_t)index * 0x45d9f3bU;
    value ^= (uint32_t)salt * 0x27d4eb2dU;
    value ^= value >> 16;
    return (uint8_t)value;
}

static void fill_pattern(uint8_t *bytes, size_t length, unsigned int salt)
{
    for (size_t i = 0; i < length; ++i) {
        bytes[i] = pattern_byte(i, salt);
    }
}

static void copy_bytes(uint8_t *dst, const uint8_t *src, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        dst[i] = src[i];
    }
}

static void assert_bytes_equal(const char *operation, size_t case_number,
                               const uint8_t *expected,
                               const uint8_t *actual, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        if (expected[i] != actual[i]) {
            fail_at(operation, case_number, i, expected[i], actual[i]);
        }
    }
}

static void assert_result_pointer(const char *operation, size_t case_number,
                                  void *expected, void *actual)
{
    if (actual != expected) {
        fprintf(stderr,
                "kernel memory test failed: %s case=%zu returned %p, "
                "expected %p\n",
                operation, case_number, actual, expected);
        exit(EXIT_FAILURE);
    }
}

static void run_small_copy_case(size_t case_number, size_t src_offset,
                                size_t dst_offset, size_t length)
{
    uint8_t source[SMALL_CAPACITY];
    uint8_t source_before[SMALL_CAPACITY];
    uint8_t destination[SMALL_CAPACITY];
    uint8_t expected[SMALL_CAPACITY];

    fill_pattern(source, sizeof(source), (unsigned int)(case_number + 11));
    fill_pattern(destination, sizeof(destination), (unsigned int)(case_number + 37));
    copy_bytes(source_before, source, sizeof(source));
    copy_bytes(expected, destination, sizeof(destination));
    copy_bytes(expected + dst_offset, source + src_offset, length);

    void *result = kernel_memcpy(destination + dst_offset, source + src_offset,
                                 length);
    assert_result_pointer("memcpy return", case_number,
                          destination + dst_offset, result);
    assert_bytes_equal("memcpy destination/sentinels", case_number, expected,
                       destination, sizeof(destination));
    assert_bytes_equal("memcpy source preservation", case_number,
                       source_before, source, sizeof(source));
}

static void run_small_set_case(size_t case_number, size_t dst_offset,
                               size_t length, int value)
{
    uint8_t destination[SMALL_CAPACITY];
    uint8_t expected[SMALL_CAPACITY];
    uint8_t byte_value = (uint8_t)value;

    fill_pattern(destination, sizeof(destination), (unsigned int)(case_number + 73));
    copy_bytes(expected, destination, sizeof(destination));
    for (size_t i = 0; i < length; ++i) {
        expected[dst_offset + i] = byte_value;
    }

    void *result = kernel_memset(destination + dst_offset, value, length);
    assert_result_pointer("memset return", case_number,
                          destination + dst_offset, result);
    assert_bytes_equal("memset value/sentinels", case_number, expected,
                       destination, sizeof(destination));
}

static void test_zero_length_guard_pointers(void)
{
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        fail("sysconf(_SC_PAGESIZE) failed");
    }

    size_t mapping_size = (size_t)page * 2;
    uint8_t *source = mmap(NULL, mapping_size, PROT_NONE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint8_t *destination = mmap(NULL, mapping_size, PROT_NONE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (source == MAP_FAILED || destination == MAP_FAILED) {
        fail("mmap failed for zero-length guard case");
    }

    void *copy_result = kernel_memcpy(destination, source, 0);
    assert_result_pointer("zero-length memcpy return", 0, destination,
                          copy_result);
    void *set_result = kernel_memset(destination, 0x1ff, 0);
    assert_result_pointer("zero-length memset return", 0, destination,
                          set_result);

    if (munmap(source, mapping_size) != 0 ||
        munmap(destination, mapping_size) != 0) {
        fail("munmap failed for zero-length guard case");
    }
}

static GuardedRegion guarded_region_create(void)
{
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        fail("sysconf(_SC_PAGESIZE) failed");
    }

    GuardedRegion region;
    region.page_size = (size_t)page;
    region.data_size = region.page_size * 2;
    region.mapping_size = region.page_size * 4;
    region.mapping = mmap(NULL, region.mapping_size, PROT_NONE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region.mapping == MAP_FAILED) {
        fail("mmap failed for guarded region");
    }
    region.data = region.mapping + region.page_size;
    if (mprotect(region.data, region.data_size, PROT_READ | PROT_WRITE) != 0) {
        fail("mprotect failed for guarded region");
    }
    return region;
}

static void guarded_region_destroy(GuardedRegion *region)
{
    if (munmap(region->mapping, region->mapping_size) != 0) {
        fail("munmap failed for guarded region");
    }
}

static void run_guarded_copy_case(GuardedRegion *source,
                                  GuardedRegion *destination,
                                  size_t case_number, size_t length,
                                  int source_at_end, int destination_at_end)
{
    uint8_t *expected = malloc(destination->data_size);
    uint8_t *source_before = malloc(source->data_size);
    if (expected == NULL || source_before == NULL) {
        fail("malloc failed for guarded copy expectation");
    }

    fill_pattern(source->data, source->data_size,
                 (unsigned int)(case_number + 101));
    fill_pattern(destination->data, destination->data_size,
                 (unsigned int)(case_number + 149));
    copy_bytes(source_before, source->data, source->data_size);
    copy_bytes(expected, destination->data, destination->data_size);

    size_t source_offset = source_at_end ? source->data_size - length : 0;
    size_t destination_offset =
        destination_at_end ? destination->data_size - length : 0;
    copy_bytes(expected + destination_offset, source->data + source_offset,
               length);

    void *dst = destination->data + destination_offset;
    void *result = kernel_memcpy(dst, source->data + source_offset, length);
    assert_result_pointer("guarded memcpy return", case_number, dst, result);
    assert_bytes_equal("guarded memcpy destination/sentinels", case_number,
                       expected, destination->data, destination->data_size);
    assert_bytes_equal("guarded memcpy source preservation", case_number,
                       source_before, source->data, source->data_size);

    free(source_before);
    free(expected);
}

static void run_guarded_set_case(GuardedRegion *destination,
                                 size_t case_number, size_t length,
                                 int destination_at_end, int value)
{
    uint8_t *expected = malloc(destination->data_size);
    if (expected == NULL) {
        fail("malloc failed for guarded memset expectation");
    }

    fill_pattern(destination->data, destination->data_size,
                 (unsigned int)(case_number + 193));
    copy_bytes(expected, destination->data, destination->data_size);

    size_t destination_offset =
        destination_at_end ? destination->data_size - length : 0;
    for (size_t i = 0; i < length; ++i) {
        expected[destination_offset + i] = (uint8_t)value;
    }

    void *dst = destination->data + destination_offset;
    void *result = kernel_memset(dst, value, length);
    assert_result_pointer("guarded memset return", case_number, dst, result);
    assert_bytes_equal("guarded memset value/sentinels", case_number,
                       expected, destination->data, destination->data_size);

    free(expected);
}

static void test_guard_pages(void)
{
    GuardedRegion source = guarded_region_create();
    GuardedRegion destination = guarded_region_create();
    const size_t page = source.page_size;
    const size_t lengths[] = {
        1, 2, 7, 15, 31, 63, 64, 65, 127, 128, 129,
        page - 1, page, page + 1, page * 2 - 1,
    };
    size_t case_number = 0;

    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        size_t length = lengths[i];
        if (length > source.data_size) {
            continue;
        }
        for (int source_at_end = 0; source_at_end <= 1; ++source_at_end) {
            for (int destination_at_end = 0; destination_at_end <= 1;
                 ++destination_at_end) {
                run_guarded_copy_case(&source, &destination, case_number++,
                                      length, source_at_end,
                                      destination_at_end);
            }
        }
        for (int destination_at_end = 0; destination_at_end <= 1;
             ++destination_at_end) {
            run_guarded_set_case(&destination, case_number++, length,
                                 destination_at_end,
                                 (int)(0x12345600U | (unsigned int)i));
        }
    }

    guarded_region_destroy(&source);
    guarded_region_destroy(&destination);
    printf("guard pages: %zu boundary cases passed\n", case_number);
}

static void test_alignment_and_length_sweep(void)
{
    static const size_t lengths[] = {
        0, 1, 2, 3, 7, 8, 15, 16, 31, 32, 63, 64, 65, 127, 128, 129, 255, 256,
    };
    size_t case_number = 0;

    for (size_t length_index = 0;
         length_index < sizeof(lengths) / sizeof(lengths[0]);
         ++length_index) {
        size_t length = lengths[length_index];
        for (size_t src_offset = 0; src_offset < 64; ++src_offset) {
            for (size_t dst_offset = 0; dst_offset < 64; ++dst_offset) {
                run_small_copy_case(case_number, src_offset, dst_offset,
                                    length);
                run_small_set_case(case_number, dst_offset, length,
                                   (int)(0x13579b00U |
                                         (unsigned int)(case_number & 0xffU)));
                ++case_number;
            }
        }
    }

    printf("alignment/length sweep: %zu memcpy and memset cases passed\n",
           case_number);
}

static void test_all_lengths_with_offset_variation(void)
{
    size_t case_number = 0;
    for (size_t length = 0; length <= 512; ++length) {
        size_t src_offset = (length * 17U) & 63U;
        size_t dst_offset = (length * 29U + 7U) & 63U;
        run_small_copy_case(case_number, src_offset, dst_offset, length);
        run_small_set_case(case_number, dst_offset, length,
                           (int)(0xa5U ^ (unsigned int)length));
        ++case_number;
    }
    printf("length sweep: %zu memcpy and memset lengths passed\n", case_number);
}

static void run_random_copy_case(size_t case_number, size_t src_offset,
                                 size_t dst_offset, size_t length)
{
    uint8_t source[RANDOM_CAPACITY];
    uint8_t source_before[RANDOM_CAPACITY];
    uint8_t destination[RANDOM_CAPACITY];
    uint8_t expected[RANDOM_CAPACITY];

    fill_pattern(source, sizeof(source), (unsigned int)(next_random() | 1U));
    fill_pattern(destination, sizeof(destination),
                 (unsigned int)(next_random() | 1U));
    copy_bytes(source_before, source, sizeof(source));
    copy_bytes(expected, destination, sizeof(destination));
    copy_bytes(expected + dst_offset, source + src_offset, length);

    void *result = kernel_memcpy(destination + dst_offset, source + src_offset,
                                 length);
    assert_result_pointer("random memcpy return", case_number,
                          destination + dst_offset, result);
    assert_bytes_equal("random memcpy destination/sentinels", case_number,
                       expected, destination, sizeof(destination));
    assert_bytes_equal("random memcpy source preservation", case_number,
                       source_before, source, sizeof(source));
}

static void run_random_set_case(size_t case_number, size_t dst_offset,
                                size_t length, int value)
{
    uint8_t destination[RANDOM_CAPACITY];
    uint8_t expected[RANDOM_CAPACITY];

    fill_pattern(destination, sizeof(destination),
                 (unsigned int)(next_random() | 1U));
    copy_bytes(expected, destination, sizeof(destination));
    for (size_t i = 0; i < length; ++i) {
        expected[dst_offset + i] = (uint8_t)value;
    }

    void *result = kernel_memset(destination + dst_offset, value, length);
    assert_result_pointer("random memset return", case_number,
                          destination + dst_offset, result);
    assert_bytes_equal("random memset value/sentinels", case_number, expected,
                       destination, sizeof(destination));
}

static void test_random_cases(void)
{
    const size_t case_count = 2048;
    for (size_t case_number = 0; case_number < case_count; ++case_number) {
        size_t length = next_random() % (RANDOM_MAX_LENGTH + 1U);
        size_t src_offset = next_random() & 63U;
        size_t dst_offset = next_random() & 63U;
        int value = (int)next_random();
        run_random_copy_case(case_number, src_offset, dst_offset, length);
        run_random_set_case(case_number, dst_offset, length, value);
    }
    printf("deterministic random cases: %zu memcpy and memset pairs passed\n",
           case_count);
}

static void test_large_blocks(void)
{
    const size_t length = 4U * 1024U * 1024U + 37U;
    const size_t offset = 63;
    size_t capacity = length + offset + 64;
    uint8_t *source = malloc(capacity);
    uint8_t *source_before = malloc(capacity);
    uint8_t *destination = malloc(capacity);
    uint8_t *expected = malloc(capacity);
    if (source == NULL || source_before == NULL || destination == NULL ||
        expected == NULL) {
        fail("malloc failed for large copy case");
    }

    fill_pattern(source, capacity, 0x2468U);
    fill_pattern(destination, capacity, 0xace1U);
    copy_bytes(source_before, source, capacity);
    copy_bytes(expected, destination, capacity);
    copy_bytes(expected + offset, source + 17, length);
    void *result = kernel_memcpy(destination + offset, source + 17, length);
    assert_result_pointer("large memcpy return", 0, destination + offset,
                          result);
    assert_bytes_equal("large memcpy destination/sentinels", 0, expected,
                       destination, capacity);
    assert_bytes_equal("large memcpy source preservation", 0, source_before,
                       source, capacity);

    fill_pattern(destination, capacity, 0x55aaU);
    copy_bytes(expected, destination, capacity);
    int value = 0x123456e7;
    for (size_t i = 0; i < length; ++i) {
        expected[offset + i] = (uint8_t)value;
    }
    result = kernel_memset(destination + offset, value, length);
    assert_result_pointer("large memset return", 0, destination + offset,
                          result);
    assert_bytes_equal("large memset value/sentinels", 0, expected,
                       destination, capacity);

    free(expected);
    free(destination);
    free(source_before);
    free(source);
    printf("large blocks: %zu-byte memcpy and memset passed\n", length);
}

int main(void)
{
    test_zero_length_guard_pointers();
    test_alignment_and_length_sweep();
    test_all_lengths_with_offset_variation();
    test_random_cases();
    test_guard_pages();
    test_large_blocks();
    puts("kernel memcpy/memset safety tests passed");
    return 0;
}
