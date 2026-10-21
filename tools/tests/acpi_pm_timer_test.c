#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/reliefnt/arch/x86_64/platform/power.c"

static uint32_t fixture_count;
static unsigned reads;
uint32_t x86_64_inl(uint16_t port)
{ assert(port == acpi_pm_timer_port); ++reads; return fixture_count; }
static void write32(uint8_t *buffer, unsigned offset, uint32_t value)
{ for (unsigned i = 0; i < 4; ++i) buffer[offset + i] = (uint8_t)(value >> (i * 8)); }
static void valid_legacy(uint8_t *fadt)
{
    memset(fadt, 0, 256); write32(fadt, 76, 0x608); fadt[91] = 4;
}
int main(void)
{
    uint8_t fadt[256]; uint32_t count, mask;
    valid_legacy(fadt); acpi_pm_timer_init(fadt, 116);
    fixture_count = 0xfedcba98;
    assert(power_acpi_pm_timer_read(&count, &mask));
    assert(mask == 0xffffff && count == 0xdcba98 && acpi_pm_timer_port == 0x608);
    write32(fadt, 112, 1u << 8); acpi_pm_timer_init(fadt, 116);
    assert(power_acpi_pm_timer_read(&count, &mask) && mask == UINT32_MAX && count == fixture_count);
    /* Extended GAS takes priority, but only supported 32-bit IO access is used. */
    fadt[208] = 1; fadt[209] = 32; fadt[211] = 3; write32(fadt, 212, 0x1008);
    acpi_pm_timer_init(fadt, 220); assert(acpi_pm_timer_port == 0x1008);
    unsigned before = reads;
    for (unsigned fault = 0; fault < 10; ++fault) {
        valid_legacy(fadt);
        switch (fault) {
        case 0: fadt[91] = 0; break;
        case 1: write32(fadt, 112, 1u << 20); break;
        case 2: write32(fadt, 76, 0); break;
        case 3: write32(fadt, 76, 0xfffd); break;
        case 4: fadt[208] = 0; fadt[209] = 32; write32(fadt, 212, 0x1008); break;
        case 5: fadt[208] = 1; fadt[209] = 32; fadt[210] = 1; write32(fadt, 212, 0x1008); break;
        case 6: fadt[208] = 1; fadt[209] = 32; fadt[211] = 2; write32(fadt, 212, 0x1008); break;
        case 7: fadt[208] = 1; fadt[209] = 32; write32(fadt, 216, 1); break;
        case 8: fadt[208] = 1; fadt[209] = 24; write32(fadt, 112, 1u << 8); write32(fadt, 212, 0x1008); break;
        case 9: fadt[208] = 1; fadt[209] = 24; write32(fadt, 212, 0x1008); break;
        }
        acpi_pm_timer_init(fadt, 220);
        assert(!power_acpi_pm_timer_read(&count, &mask));
    }
    valid_legacy(fadt); acpi_pm_timer_init(fadt, 115);
    assert(!power_acpi_pm_timer_read(&count, &mask));
    assert(reads == before); /* invalid firmware must cause no port read */
    puts("PASS ACPI PM clock: legacy/GAS priority, width/mask, malformed and unsupported descriptors");
    return 0;
}
