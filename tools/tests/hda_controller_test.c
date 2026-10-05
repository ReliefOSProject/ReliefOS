#define HDA_TESTING 1
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefos/driver.h>

#include "../../kernel/reliefnt/drivers/hda/hda.h"

struct hda_fake_device;
static uint8_t hda_test_mmio_read8(void *base, uint32_t offset);
static uint16_t hda_test_mmio_read16(void *base, uint32_t offset);
static uint32_t hda_test_mmio_read32(void *base, uint32_t offset);
static void hda_test_mmio_write8(void *base, uint32_t offset, uint8_t value);
static void hda_test_mmio_write16(void *base, uint32_t offset, uint16_t value);
static void hda_test_mmio_write32(void *base, uint32_t offset, uint32_t value);
static void *hda_test_dma_map(uint64_t phys);
static uint64_t hda_test_irq_save(void);
static void hda_test_irq_restore(uint64_t flags);
static void hda_test_before_sleep(const struct hda_controller *c);

#include "../../kernel/reliefnt/drivers/hda/controller.c"
#include "../../kernel/reliefnt/drivers/hda/controls.c"

#define FAKE_REGS_BYTES 0x4000u
#define FAKE_PAGE_BYTES 4096u
#define FAKE_BAR_BASE 0x10000000u
#define FAKE_BAR_BYTES 0x4000u
#define FAKE_DMA_BASE 0x00200000u

struct hda_fake_dma {
    uint64_t phys;
    void *memory;
    uint32_t pages;
    uint8_t live;
};

struct hda_fake_device {
    uint8_t regs[FAKE_REGS_BYTES];
    uint32_t pci[64];
    uint32_t bar_probe[2];
    struct hda_fake_dma dma[4];
    uint64_t ticks;
    uint32_t phase_us;
    uint64_t sleep_elapsed_us;
    uint32_t sleep_calls;
    uint32_t alloc_calls;
    uint32_t successful_alloc_calls;
    uint32_t free_calls;
    uint32_t map_calls;
    uint32_t unmap_calls;
    uint32_t irq_calls;
    uint32_t irq_frees;
    uint32_t live_dma;
    uint32_t live_maps;
    uint32_t live_irqs;
    uint32_t ring_entries;
    uint32_t fake_rirb_wp;
    uint32_t qemu_response_limit;
    uint32_t responses_since_ack;
    uint32_t if_enabled;
    uint32_t sleep_entry_if_enabled;
    uint32_t irq_progress;
    uint32_t sleep_under_lock;
    uint32_t delay_corb_stop_ticks;
    uint32_t delay_rirb_stop_ticks;
    uint32_t delay_crst_low_ticks;
    uint32_t delay_crst_high_ticks;
    uint64_t corb_stop_due_tick;
    uint64_t rirb_stop_due_tick;
    uint64_t crst_due_tick;
    uint64_t crst_high_tick;
    int32_t pending_crst_target;
    uint32_t pending_corb_stop;
    uint32_t pending_rirb_stop;
    uint32_t pending_crst;
    uint32_t crst_low_while_dma_running;
    uint32_t ring_write_while_reset;
    uint32_t ring_write_before_stable;
    uint32_t auto_response;
    uint32_t response_value;
    uint32_t fail_alloc_at;
    uint32_t fail_map;
    uint32_t fail_irq;
    uint32_t fail_corb_start;
    uint32_t fail_rirb_start;
    uint32_t fail_bus_master_reenable;
    uint32_t stuck_crst_clear;
    uint32_t stuck_crst_set;
    uint32_t stuck_corb_reset;
    uint32_t stuck_rirb_wp_reset;
    uint32_t stuck_rirb_stop;
    uint32_t reset_count;
    uint32_t stale_pending;
    uint32_t stale_cad;
    uint32_t stale_response;
    uint32_t defer_next_response;
    uint32_t stale_discarded;
    uint32_t free_while_dma_live;
    uint32_t immediate_present;
    uint32_t immediate_version;
    uint32_t immediate_auto_response;
    uint32_t immediate_no_progress;
    uint32_t immediate_stuck_valid;
    uint32_t immediate_defer_response;
    uint32_t immediate_late_pending;
    uint32_t immediate_late_cad;
    uint32_t immediate_late_response;
    uint32_t immediate_override;
    uint32_t immediate_override_cad;
    uint32_t immediate_override_unsolicited;
    uint32_t immediate_override_response;
    uint32_t immediate_command_count;
    uint32_t immediate_with_rings;
    uint32_t immediate_reset_discarded;
    uint32_t console_calls;
    char last_log[128];
    void (*irq_handler)(void *);
    void *irq_opaque;
};

static struct hda_fake_device fake;
static uint32_t hda_total_sleep_under_lock;
static const struct reliefos_driver_pci_device fake_pci = {
    .bus = 0, .slot = 27, .function = 0,
    .class_code = 4, .subclass = 3, .vendor_id = 0x8086, .device_id = 0x2668,
};

static uint16_t fake_pci_read16(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset);
static uint32_t fake_pci_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset);
static void fake_pci_write16(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint16_t value);
static void fake_pci_write32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint32_t value);
static void *fake_map_mmio(uint64_t phys, uint64_t bytes);
static void fake_unmap_mmio(void *ptr, uint64_t bytes);
static uint64_t fake_alloc_dma(uint32_t pages, uint64_t mask);
static void fake_free_dma(uint64_t phys, uint32_t pages);
static int fake_request_irq(const struct reliefos_driver_pci_device *dev,
                            void (*handler)(void *), void *opaque, uint32_t *out_handle);
static void fake_free_irq(uint32_t handle);
static uint64_t fake_ticks(void);
static void fake_sleep_ms(uint64_t ms);
static void fake_console(const char *text);
static void fake_push_response(uint8_t cad, uint32_t response, int unsolicited);
static uint16_t fake_raw16(const struct hda_fake_device *dev, uint32_t offset);

static uint32_t fake_raw32(const struct hda_fake_device *dev, uint32_t offset) {
    return (uint32_t)dev->regs[offset] | ((uint32_t)dev->regs[offset + 1u] << 8) |
           ((uint32_t)dev->regs[offset + 2u] << 16) |
           ((uint32_t)dev->regs[offset + 3u] << 24);
}

static void fake_apply_controller_reset(struct hda_fake_device *dev) {
    uint8_t gcap_low = dev->regs[0];
    uint8_t gcap_high = dev->regs[1];
    uint8_t corb_caps = dev->regs[0x4e] & 0xf0u;
    uint8_t rirb_caps = dev->regs[0x5e] & 0xf0u;
    uint16_t wakeen = fake_raw16(dev, 0x0c);
    uint16_t statests = fake_raw16(dev, 0x0e);
    uint16_t immediate_status = fake_raw16(dev, 0x68);
    if (immediate_status & ((1u << 0) | (1u << 1)))
        ++dev->immediate_reset_discarded;
    dev->immediate_late_pending = 0u;
    memset(dev->regs, 0, sizeof(dev->regs));
    dev->regs[0] = gcap_low;
    dev->regs[1] = gcap_high;
    dev->regs[0x0c] = (uint8_t)wakeen;
    dev->regs[0x0d] = (uint8_t)(wakeen >> 8);
    dev->regs[0x0e] = (uint8_t)statests;
    dev->regs[0x0f] = (uint8_t)(statests >> 8);
    dev->regs[0x4e] = corb_caps;
    dev->regs[0x5e] = rirb_caps;
    dev->regs[0x68] = dev->immediate_version ? (1u << 2) : 0u;
    dev->fake_rirb_wp = 0u;
    dev->stale_discarded += dev->stale_pending;
    dev->stale_pending = 0u;
    ++dev->reset_count;
}

static void fake_update_hardware(struct hda_fake_device *dev) {
    if (dev->pending_corb_stop && dev->ticks >= dev->corb_stop_due_tick) {
        dev->regs[0x4c] &= (uint8_t)~2u;
        dev->pending_corb_stop = 0u;
    }
    if (dev->pending_rirb_stop && dev->ticks >= dev->rirb_stop_due_tick) {
        dev->regs[0x5c] &= (uint8_t)~2u;
        dev->pending_rirb_stop = 0u;
    }
    if (!dev->pending_crst || dev->ticks < dev->crst_due_tick) return;
    dev->pending_crst = 0u;
    if (dev->pending_crst_target == 0) {
        fake_apply_controller_reset(dev);
    } else {
        dev->regs[0x08] |= 1u;
        dev->crst_high_tick = dev->ticks;
    }
}

static int fake_write_blocked_in_reset(struct hda_fake_device *dev,
                                       uint32_t offset, uint32_t width) {
    fake_update_hardware(dev);
    if (fake_raw32(dev, 0x08) & 1u) {
        if (dev->reset_count && offset >= 0x40u && offset <= 0x5eu &&
            dev->ticks - dev->crst_high_tick < HDA_RECOVERY_STABLE_TICKS)
            ++dev->ring_write_before_stable;
        return 0;
    }
    if (offset == 0x08u || (offset == 0x0cu && width == 2u) ||
        (offset == 0x0eu && width == 2u)) return 0;
    if ((offset >= 0x20u && offset <= 0x69u) || offset == 0x70u || offset == 0x74u)
        ++dev->ring_write_while_reset;
    return 1;
}

static uint16_t fake_raw16(const struct hda_fake_device *dev, uint32_t offset) {
    return (uint16_t)dev->regs[offset] | ((uint16_t)dev->regs[offset + 1u] << 8);
}

static void fake_refresh_intsts(struct hda_fake_device *dev) {
    uint8_t corb_control = dev->regs[0x4c];
    uint8_t corb_status = dev->regs[0x4d];
    uint8_t rirb_control = dev->regs[0x5c];
    uint8_t rirb_status = dev->regs[0x5d];
    uint16_t wake_enable = fake_raw16(dev, 0x0c);
    uint16_t state_status = fake_raw16(dev, 0x0e);
    uint32_t controller_source =
        ((corb_status & 1u) && (corb_control & 1u)) ||
        ((rirb_status & (1u << 0)) && (rirb_control & (1u << 0))) ||
        ((rirb_status & (1u << 2)) && (rirb_control & (1u << 2))) ||
        (wake_enable & state_status);
    uint32_t status = controller_source ? (1u << 30) : 0u;
    if (status) status |= 1u << 31;
    dev->regs[0x24] = (uint8_t)status;
    dev->regs[0x25] = (uint8_t)(status >> 8);
    dev->regs[0x26] = (uint8_t)(status >> 16);
    dev->regs[0x27] = (uint8_t)(status >> 24);
}

static int fake_irq_asserted(void) {
    fake_refresh_intsts(&fake);
    uint32_t status = fake_raw32(&fake, 0x24);
    uint32_t control = fake_raw32(&fake, 0x20);
    return (status & (1u << 31)) && (control & (1u << 31)) &&
           (status & (1u << 30)) && (control & (1u << 30));
}

static void fake_deliver_irq(void) {
    if (fake_irq_asserted() && fake.irq_handler)
        fake.irq_handler(fake.irq_opaque);
}

static struct reliefos_driver_kernel_api fake_api(void) {
    return (struct reliefos_driver_kernel_api){
        .abi_version = RELIEFOS_DRIVER_ABI_VERSION,
        .struct_size = sizeof(struct reliefos_driver_kernel_api),
        .console_write = fake_console,
        .pci_read16 = fake_pci_read16,
        .pci_write16 = fake_pci_write16,
        .pci_read32 = fake_pci_read32,
        .pci_write32 = fake_pci_write32,
        .ticks = fake_ticks,
        .sleep_ms = fake_sleep_ms,
        .map_mmio = fake_map_mmio,
        .unmap_mmio = fake_unmap_mmio,
        .alloc_dma = fake_alloc_dma,
        .free_dma = fake_free_dma,
        .request_pci_irq = fake_request_irq,
        .free_pci_irq = fake_free_irq,
    };
}

static void fake_device_init(uint8_t corb_caps, uint8_t rirb_caps) {
    memset(&fake, 0, sizeof(fake));
    fake.pci[1] = 0x00000007u; /* command/status dword */
    fake.pci[4] = FAKE_BAR_BASE;
    fake.bar_probe[0] = 0;
    fake.bar_probe[1] = 0;
    fake.regs[0x00] = 1; /* GCAP.64OK */
    fake.regs[0x01] = 0;
    fake.regs[0x4e] = corb_caps;
    fake.regs[0x5e] = rirb_caps;
    fake.ring_entries = (corb_caps & 0x40u) ? 256u :
                        (corb_caps & 0x20u) ? 16u : 2u;
    fake.auto_response = 1;
    fake.response_value = 0x13579bdfu;
    fake.phase_us = 9999u; /* Exercise the real 100 Hz sleep rounding boundary. */
    fake.immediate_present = 1u;
    fake.immediate_version = 1u;
    fake.immediate_auto_response = 1u;
}

static void fake_assert_resources_reclaimed(void) {
    assert(fake.live_dma == 0);
    assert(fake.live_maps == 0);
    assert(fake.live_irqs == 0);
    assert(fake.free_calls == fake.successful_alloc_calls);
}

static void assert_ring_register_publication(const struct hda_controller *c) {
    assert(hda_test_mmio_read16(&fake, HDA_REG_CORBWP) == 0u);
    assert(hda_test_mmio_read32(&fake, HDA_REG_CORBLBASE) == (uint32_t)c->corb_phys);
    assert(hda_test_mmio_read32(&fake, HDA_REG_CORBUBASE) == (uint32_t)(c->corb_phys >> 32));
    assert((hda_test_mmio_read8(&fake, HDA_REG_CORBSIZE) & 3u) == c->corb_size_select);
    assert(hda_test_mmio_read32(&fake, HDA_REG_RIRBLBASE) == (uint32_t)c->rirb_phys);
    assert(hda_test_mmio_read32(&fake, HDA_REG_RIRBUBASE) == (uint32_t)(c->rirb_phys >> 32));
    assert((hda_test_mmio_read8(&fake, HDA_REG_RIRBSIZE) & 3u) == c->rirb_size_select);
    assert(hda_test_mmio_read16(&fake, HDA_REG_RINTCNT) == 1u);
    assert(hda_test_mmio_read32(&fake, HDA_REG_DPLBASE) == 0u);
    assert(hda_test_mmio_read32(&fake, HDA_REG_DPUBASE) == 0u);
    assert((hda_test_mmio_read8(&fake, HDA_REG_CORBCTL) & HDA_CORB_RUN) != 0u);
    assert((hda_test_mmio_read8(&fake, HDA_REG_RIRBCTL) & HDA_RIRB_DMA_ENABLE) != 0u);
    assert((hda_test_mmio_read8(&fake, HDA_REG_CORBCTL) & HDA_CORB_MEMORY_ERROR_IRQ) ==
           (c->owns_irq ? HDA_CORB_MEMORY_ERROR_IRQ : 0u));
    assert((hda_test_mmio_read8(&fake, HDA_REG_RIRBCTL) &
            (HDA_RIRB_RESPONSE_IRQ | HDA_RIRB_OVERRUN_IRQ)) ==
           (HDA_RIRB_RESPONSE_IRQ | (c->owns_irq ? HDA_RIRB_OVERRUN_IRQ : 0u)));
    assert((hda_test_mmio_read32(&fake, HDA_REG_INTCTL) & 0xc0000000u) ==
           (c->owns_irq ? 0xc0000000u : 0u));
    assert(hda_test_mmio_read32(&fake, HDA_REG_GCTL) & HDA_GCTL_UNSOL);
}

static void fake_push_response(uint8_t cad, uint32_t response, int unsolicited) {
    if (!(fake.regs[0x5c] & 2u)) return; /* RIRB DMA is the delivery gate. */
    if (unsolicited && !(hda_test_mmio_read32(&fake, 0x08) & (1u << 8))) return;
    uint64_t phys = (uint64_t)hda_test_mmio_read32(&fake, 0x50) |
                    ((uint64_t)hda_test_mmio_read32(&fake, 0x54) << 32);
    volatile uint64_t *rirb = hda_test_dma_map(phys);
    assert(rirb && "fake RIRB base must resolve to an active DMA lease");
    uint32_t entries = fake.ring_entries;
    fake.fake_rirb_wp = (fake.fake_rirb_wp + 1u) % entries;
    uint64_t ext = (uint64_t)(cad & 0x0fu) | (unsolicited ? (1ULL << 4) : 0);
    rirb[fake.fake_rirb_wp] = ((uint64_t)response) | (ext << 32);
    *(uint16_t *)(void *)&fake.regs[0x58] = (uint16_t)fake.fake_rirb_wp;
    if (fake.regs[0x5c] & 1u) fake.regs[0x5d] |= 1u;
    ++fake.responses_since_ack;
}

static void fake_store_immediate_status(uint16_t status) {
    fake.regs[0x68] = (uint8_t)status;
    fake.regs[0x69] = (uint8_t)(status >> 8);
}

static void fake_complete_immediate(struct hda_fake_device *dev, uint8_t cad,
                                    uint32_t response, int unsolicited) {
    uint16_t status = fake_raw16(dev, 0x68);
    if (!(status & (1u << 0))) return;
    status &= (uint16_t)~(1u << 0);
    if (unsolicited && !(fake_raw32(dev, 0x08) & (1u << 8))) {
        fake_store_immediate_status(status);
        return; /* GCTL.UNSOL gates unsolicited delivery in immediate mode too. */
    }
    dev->regs[0x64] = (uint8_t)response;
    dev->regs[0x65] = (uint8_t)(response >> 8);
    dev->regs[0x66] = (uint8_t)(response >> 16);
    dev->regs[0x67] = (uint8_t)(response >> 24);
    if (dev->immediate_version) {
        status &= (uint16_t)~(0xf8u);
        status |= (uint16_t)((cad & 0x0fu) << 4);
        if (unsolicited) status |= 1u << 3;
        status |= 1u << 2;
    } else {
        status |= 0x00f8u; /* Reserved legacy bits are deliberately unusable. */
    }
    status |= 1u << 1;
    fake_store_immediate_status(status);
}

static uint8_t hda_test_mmio_read8(void *base, uint32_t offset) {
    struct hda_fake_device *dev = base;
    assert(offset < FAKE_REGS_BYTES);
    fake_update_hardware(dev);
    return dev->regs[offset];
}

static uint16_t hda_test_mmio_read16(void *base, uint32_t offset) {
    struct hda_fake_device *dev = base;
    assert(offset + 2u <= FAKE_REGS_BYTES);
    fake_update_hardware(dev);
    return (uint16_t)dev->regs[offset] | ((uint16_t)dev->regs[offset + 1u] << 8);
}

static uint32_t hda_test_mmio_read32(void *base, uint32_t offset) {
    struct hda_fake_device *dev = base;
    assert(offset + 4u <= FAKE_REGS_BYTES);
    fake_update_hardware(dev);
    if (offset == 0x24u) fake_refresh_intsts(dev);
    return (uint32_t)dev->regs[offset] | ((uint32_t)dev->regs[offset + 1u] << 8) |
           ((uint32_t)dev->regs[offset + 2u] << 16) | ((uint32_t)dev->regs[offset + 3u] << 24);
}

static void hda_test_mmio_write8(void *base, uint32_t offset, uint8_t value) {
    struct hda_fake_device *dev = base;
    assert(offset < FAKE_REGS_BYTES);
    if (fake_write_blocked_in_reset(dev, offset, 1u)) return;
    if (offset == 0x4d || offset == 0x5d) {
        if (offset == 0x5d && (dev->regs[offset] & value & 1u))
            dev->responses_since_ack = 0u;
        dev->regs[offset] &= (uint8_t)~value; /* CORB/RIRB status are W1C. */
        return;
    }
    if (offset == 0x4c && dev->fail_corb_start && (value & 2u)) return;
    if (offset == 0x5c && dev->fail_rirb_start && (value & 2u)) return;
    uint8_t previous = dev->regs[offset];
    if (offset == 0x4cu && (previous & 2u) && !(value & 2u) &&
        dev->delay_corb_stop_ticks) {
        dev->pending_corb_stop = 1u;
        dev->corb_stop_due_tick = dev->ticks + dev->delay_corb_stop_ticks;
        dev->regs[offset] = (uint8_t)(value | 2u);
        return;
    }
    if (offset == 0x5cu && (previous & 2u) && !(value & 2u) &&
        dev->delay_rirb_stop_ticks) {
        dev->pending_rirb_stop = 1u;
        dev->rirb_stop_due_tick = dev->ticks + dev->delay_rirb_stop_ticks;
        dev->regs[offset] = (uint8_t)(value | 2u);
        return;
    }
    if (offset == 0x5c && dev->stuck_rirb_stop && (previous & 2u) &&
        !(value & 2u)) {
        dev->regs[offset] = (uint8_t)(value | 2u);
        return;
    }
    dev->regs[offset] = value;
}

static void hda_test_mmio_write16(void *base, uint32_t offset, uint16_t value) {
    struct hda_fake_device *dev = base;
    assert(offset + 2u <= FAKE_REGS_BYTES);
    if (fake_write_blocked_in_reset(dev, offset, 2u)) return;
    if (offset == HDA_REG_ICIS) {
        if (!dev->immediate_present) return;
        uint16_t status = fake_raw16(dev, HDA_REG_ICIS);
        if ((value & HDA_ICIS_IRV) && !dev->immediate_stuck_valid)
            status &= (uint16_t)~HDA_ICIS_IRV;
        status = (uint16_t)((status & (uint16_t)~HDA_ICIS_ICVER) |
                            (dev->immediate_version ? HDA_ICIS_ICVER : 0u));
        if (value & HDA_ICIS_ICB) {
            if ((dev->regs[0x4c] & HDA_CORB_RUN) ||
                (dev->regs[0x5c] & HDA_RIRB_DMA_ENABLE)) {
                ++dev->immediate_with_rings;
                fake_store_immediate_status(status);
                return;
            }
            if (status & HDA_ICIS_ICB) {
                fake_store_immediate_status(status);
                return;
            }
            status |= HDA_ICIS_ICB;
            fake_store_immediate_status(status);
            ++dev->immediate_command_count;
            if (dev->immediate_defer_response) {
                uint32_t command = fake_raw32(dev, HDA_REG_ICOI);
                dev->immediate_defer_response = 0u;
                dev->immediate_late_pending = 1u;
                dev->immediate_late_cad = (uint8_t)(command >> 28);
                dev->immediate_late_response = 0x0badc0deu;
            } else if (!dev->immediate_no_progress && dev->immediate_auto_response) {
                uint32_t command = fake_raw32(dev, HDA_REG_ICOI);
                uint8_t cad = dev->immediate_override ?
                              (uint8_t)dev->immediate_override_cad :
                              (uint8_t)(command >> 28);
                uint32_t response = dev->immediate_override ?
                                    dev->immediate_override_response : dev->response_value;
                int unsolicited = dev->immediate_override_unsolicited != 0u;
                fake_complete_immediate(dev, cad, response, unsolicited);
            }
            return;
        }
        fake_store_immediate_status(status);
        return;
    }
    if (offset == 0x0e) {
        uint16_t old = hda_test_mmio_read16(base, offset);
        value = (uint16_t)(old & (uint16_t)~value); /* STATESTS is W1C. */
    }
    if (offset == 0x4a && (value & 0x8000u)) {
        if (!dev->stuck_corb_reset) {
            dev->regs[offset] = 0u;
            dev->regs[offset + 1u] = 0x80u;
        }
        return;
    }
    if (offset == 0x4a && !(value & 0x8000u)) {
        dev->regs[offset] = 0u;
        dev->regs[offset + 1u] = 0u;
        return;
    }
    if (offset == 0x58 && (value & 0x8000u)) {
        if (dev->stuck_rirb_wp_reset) return;
        dev->fake_rirb_wp = 0;
        dev->regs[offset] = 0;
        return;
    }
    dev->regs[offset] = (uint8_t)value;
    dev->regs[offset + 1u] = (uint8_t)(value >> 8);
    if (offset == 0x48 && (dev->regs[0x4c] & 2u)) {
        if (dev->qemu_response_limit &&
            dev->responses_since_ack == fake_raw16(dev, 0x5au)) return;
        uint64_t phys = hda_test_mmio_read32(base, 0x40) |
                        ((uint64_t)hda_test_mmio_read32(base, 0x44) << 32);
        volatile uint32_t *corb = hda_test_dma_map(phys);
        uint32_t command = corb[value & 0xffu];
        uint8_t cad = (uint8_t)(command >> 28);
        dev->regs[0x4a] = (uint8_t)value;
        dev->regs[0x4b] = (uint8_t)(value >> 8);
        if (dev->stale_pending) {
            fake_push_response((uint8_t)dev->stale_cad, dev->stale_response, 0);
            --dev->stale_pending;
        }
        if (dev->defer_next_response) {
            dev->defer_next_response = 0;
            dev->stale_pending = 1;
            dev->stale_cad = cad;
            dev->stale_response = 0xdeadfa11u;
        } else if (dev->auto_response) {
            fake_push_response(cad, dev->response_value, 0);
        }
    }
}

static void hda_test_mmio_write32(void *base, uint32_t offset, uint32_t value) {
    struct hda_fake_device *dev = base;
    assert(offset + 4u <= FAKE_REGS_BYTES);
    if (offset == HDA_REG_ICOI) {
        if (dev->immediate_present && !fake_write_blocked_in_reset(dev, offset, 4u)) {
            dev->regs[offset] = (uint8_t)value;
            dev->regs[offset + 1u] = (uint8_t)(value >> 8);
            dev->regs[offset + 2u] = (uint8_t)(value >> 16);
            dev->regs[offset + 3u] = (uint8_t)(value >> 24);
        }
        return;
    }
    if (offset == HDA_REG_ICII) return; /* Immediate response input is read-only. */
    if (offset == 0x08) {
        fake_update_hardware(dev);
        uint32_t old = fake_raw32(dev, offset);
        int old_crst = (old & 1u) != 0;
        int new_crst = (value & 1u) != 0;
        if (old_crst && !new_crst) {
            if ((dev->regs[0x4c] & 2u) || (dev->regs[0x5c] & 2u)) {
                ++dev->crst_low_while_dma_running;
                return;
            }
            if (dev->stuck_crst_clear) return;
            if (dev->delay_crst_low_ticks) {
                dev->pending_crst = 1u;
                dev->pending_crst_target = 0;
                dev->crst_due_tick = dev->ticks + dev->delay_crst_low_ticks;
                return;
            }
            fake_apply_controller_reset(dev);
            return;
        }
        if (!old_crst && new_crst) {
            if (dev->stuck_crst_set) return;
            if (dev->delay_crst_high_ticks) {
                dev->pending_crst = 1u;
                dev->pending_crst_target = 1;
                dev->crst_due_tick = dev->ticks + dev->delay_crst_high_ticks;
                return;
            }
            dev->regs[0x08] = (uint8_t)(value | 1u);
            dev->regs[0x09] = (uint8_t)(value >> 8);
            dev->regs[0x0a] = (uint8_t)(value >> 16);
            dev->regs[0x0b] = (uint8_t)(value >> 24);
            dev->crst_high_tick = dev->ticks;
            return;
        }
        value = (value & ~1u) | (old_crst ? 1u : 0u);
    }
    if (offset == 0x24u) return; /* INTSTS is a read-only source summary. */
    else if (fake_write_blocked_in_reset(dev, offset, 4u)) return;
    dev->regs[offset] = (uint8_t)value;
    dev->regs[offset + 1u] = (uint8_t)(value >> 8);
    dev->regs[offset + 2u] = (uint8_t)(value >> 16);
    dev->regs[offset + 3u] = (uint8_t)(value >> 24);
}

static void *hda_test_dma_map(uint64_t phys) {
    for (uint32_t i = 0; i < 4; ++i)
        if (fake.dma[i].live && fake.dma[i].phys == phys) return fake.dma[i].memory;
    return NULL;
}

static uint64_t hda_test_irq_save(void) {
    uint64_t flags = fake.if_enabled ? (1ull << 9) : 0u;
    fake.if_enabled = 0u; /* Production pushfq; cli semantics. */
    return flags;
}
static void hda_test_irq_restore(uint64_t flags) {
    fake.if_enabled = (flags & (1ull << 9)) != 0u;
}
static void hda_test_before_sleep(const struct hda_controller *c) {
    if (c->lock.held) {
        ++fake.sleep_under_lock;
        ++hda_total_sleep_under_lock;
    }
}

static uint16_t fake_pci_read16(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    (void)bus; (void)slot; (void)function;
    uint32_t word = fake.pci[offset / 4u];
    return (uint16_t)(word >> ((offset & 2u) * 8u));
}

static uint32_t fake_pci_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    (void)bus; (void)slot; (void)function;
    if (offset == 0x10u && fake.bar_probe[0]) return fake.bar_probe[0];
    if (offset == 0x14u && fake.bar_probe[1]) return fake.bar_probe[1];
    return fake.pci[offset / 4u];
}

static void fake_pci_write16(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint16_t value) {
    (void)bus; (void)slot; (void)function;
    if (offset == HDA_PCI_COMMAND && fake.fail_bus_master_reenable && fake.free_calls)
        value &= (uint16_t)~HDA_PCI_COMMAND_MASTER;
    uint32_t *word = &fake.pci[offset / 4u];
    uint32_t shift = (offset & 2u) * 8u;
    uint32_t mask = 0xffffu << shift;
    *word = (*word & ~mask) | ((uint32_t)value << shift);
}

static void fake_pci_write32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint32_t value) {
    (void)bus; (void)slot; (void)function;
    if (offset == 0x10u && value == 0xffffffffu) { fake.bar_probe[0] = 0xffffc000u; return; }
    if (offset == 0x14u && value == 0xffffffffu) { fake.bar_probe[1] = 0; return; }
    fake.pci[offset / 4u] = value;
    if (offset == 0x10u) fake.bar_probe[0] = 0;
    if (offset == 0x14u) fake.bar_probe[1] = 0;
}

static void *fake_map_mmio(uint64_t phys, uint64_t bytes) {
    ++fake.map_calls;
    if (fake.fail_map || phys != FAKE_BAR_BASE || bytes != FAKE_BAR_BYTES) return NULL;
    ++fake.live_maps;
    return &fake;
}

static void fake_unmap_mmio(void *ptr, uint64_t bytes) {
    assert(ptr == &fake && bytes == FAKE_BAR_BYTES);
    if ((fake.regs[0x4c] & 2u) || (fake.regs[0x5c] & 2u)) fake.free_while_dma_live = 1;
    ++fake.unmap_calls;
    assert(fake.live_maps);
    --fake.live_maps;
}

static uint64_t fake_alloc_dma(uint32_t pages, uint64_t mask) {
    ++fake.alloc_calls;
    if (fake.fail_alloc_at == fake.alloc_calls) return 0;
    for (uint32_t i = 0; i < 4; ++i) if (!fake.dma[i].live) {
        void *memory = NULL;
        assert(posix_memalign(&memory, FAKE_PAGE_BYTES, (size_t)pages * FAKE_PAGE_BYTES) == 0);
        memset(memory, 0, (size_t)pages * FAKE_PAGE_BYTES);
        uint64_t phys = FAKE_DMA_BASE + (uint64_t)i * 0x1000u;
        if (phys + (uint64_t)pages * FAKE_PAGE_BYTES - 1u > mask) { free(memory); return 0; }
        fake.dma[i] = (struct hda_fake_dma){phys, memory, pages, 1};
        ++fake.live_dma;
        ++fake.successful_alloc_calls;
        return phys;
    }
    return 0;
}

static void fake_free_dma(uint64_t phys, uint32_t pages) {
    for (uint32_t i = 0; i < 4; ++i) if (fake.dma[i].live && fake.dma[i].phys == phys) {
        assert(fake.dma[i].pages == pages);
        if ((fake.regs[0x4c] & 2u) || (fake.regs[0x5c] & 2u)) fake.free_while_dma_live = 1;
        free(fake.dma[i].memory);
        fake.dma[i].live = 0;
        --fake.live_dma;
        ++fake.free_calls;
        return;
    }
    assert(!"free_dma did not match an allocated lease");
}

static int fake_request_irq(const struct reliefos_driver_pci_device *dev,
                            void (*handler)(void *), void *opaque, uint32_t *out_handle) {
    (void)dev;
    ++fake.irq_calls;
    if (fake.fail_irq) return -ENOTSUP;
    fake.irq_handler = handler;
    fake.irq_opaque = opaque;
    ++fake.live_irqs;
    *out_handle = 9;
    return 0;
}

static void fake_free_irq(uint32_t handle) {
    assert(handle == 9);
    if (fake.live_irqs) --fake.live_irqs;
    ++fake.irq_frees;
}

static uint64_t fake_ticks(void) { return fake.ticks; }

/* Mirrors time_sleep_ms: ceil(ms * 100 Hz), minimum one tick, and waits for a tick edge. */
static void fake_sleep_ms(uint64_t ms) {
    uint64_t delta = (ms * 100u + 999u) / 1000u;
    if (!delta) delta = 1;
    ++fake.sleep_calls;
    fake.sleep_entry_if_enabled = fake.if_enabled;
    fake.if_enabled = 1u; /* H1 uses STI/HLT while waiting. */
    uint64_t end = fake.ticks + delta;
    while (fake.ticks < end) {
        fake.sleep_elapsed_us += 10000u - fake.phase_us;
        ++fake.ticks;
        ++fake.irq_progress;
        fake.phase_us = 0;
    }
    fake.if_enabled = 0u; /* H1 returns through CLI. */
}

static void fake_console(const char *text) {
    ++fake.console_calls;
    uint32_t i = 0u;
    while (text && text[i] && i + 1u < sizeof(fake.last_log)) {
        fake.last_log[i] = text[i];
        ++i;
    }
    fake.last_log[i] = '\0';
}

static void verb_layout_is_correct(void) {
    assert(hda_encode_verb(2, 0x12, 0xf00, 9, false) == 0x212f0009u);
    assert(hda_encode_verb(1, 3, 3, 0xb080, true) == 0x1033b080u);
}

static void sleep_preserves_if_state_and_allows_interrupt_progress(void) {
    struct reliefos_driver_kernel_api api = fake_api();
    struct hda_controller c = {.api = &api};
    uint64_t flags;
    fake_device_init(0x40, 0x40);

    fake.if_enabled = 0u;
    flags = hda_lock(&c);
    assert(!fake.if_enabled && c.lock.held);
    hda_unlock(&c, flags);
    assert(!fake.if_enabled && !c.lock.held);
    fake.if_enabled = 1u;
    flags = hda_lock(&c);
    assert(!fake.if_enabled && c.lock.held);
    hda_unlock(&c, flags);
    assert(fake.if_enabled && !c.lock.held);

    fake.if_enabled = 0u;
    hda_controller_sleep_ms(&c, 1u);
    assert(fake.sleep_entry_if_enabled == 0u);
    assert(fake.if_enabled == 0u);

    fake.if_enabled = 1u;
    hda_controller_sleep_ms(&c, 1u);
    assert(fake.sleep_entry_if_enabled == 1u);
    assert(fake.if_enabled == 1u);
    assert(fake.irq_progress == 2u);
    assert(fake.sleep_under_lock == 0u);
}

static void reset_failure_releases_every_acquired_lease(void) {
    struct reliefos_driver_kernel_api api;
    struct hda_controller c;
    for (uint32_t fail_at = 1; fail_at <= 2; ++fail_at) {
        fake_device_init(0x40, 0x40);
        api = fake_api();
        fake.fail_alloc_at = fail_at;
        assert(hda_controller_init(&c, &api, &fake_pci) < 0);
        assert(fake.console_calls == 0u);
        fake_assert_resources_reclaimed();
        assert(!fake.free_while_dma_live);
        assert(fake_pci_read16(0, 27, 0, 4) == 7);
    }

    fake_device_init(0x40, 0x40);
    fake.fail_map = 1;
    api = fake_api();
    assert(hda_controller_init(&c, &api, &fake_pci) == -ENOMEM);
    assert(fake.console_calls == 0u);
    fake_assert_resources_reclaimed();
    assert(fake_pci_read16(0, 27, 0, 4) == 7);

    fake_device_init(0x40, 0x40);
    fake.pci[4] = 0u;
    api = fake_api();
    assert(hda_controller_init(&c, &api, &fake_pci) == -ENODEV);
    fake_assert_resources_reclaimed();
    assert(fake_pci_read16(0, 27, 0, 4) == 7);
}

static void ring_sizes_follow_capabilities(void) {
    static const struct { uint8_t caps; uint32_t entries; uint8_t select; } cases[] = {
        {0x10, 2, 0}, {0x20, 16, 1}, {0x40, 256, 2},
        {0x50, 256, 2}, {0x60, 256, 2}, {0x70, 256, 2},
    };
    struct reliefos_driver_kernel_api api = fake_api();
    struct hda_controller c;
    for (uint32_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        fake_device_init(cases[i].caps, cases[i].caps);
        api = fake_api();
        assert(hda_controller_init(&c, &api, &fake_pci) == 0);
        assert(fake.sleep_elapsed_us >= 1500u);
        assert(c.corb_entries == cases[i].entries && c.rirb_entries == cases[i].entries);
        assert((fake.regs[0x4e] & 3u) == cases[i].select);
        assert((fake.regs[0x5e] & 3u) == cases[i].select);
        assert_ring_register_publication(&c);
        assert(hda_controller_destroy(&c) == 0);
        assert(!fake.free_while_dma_live);
        fake_assert_resources_reclaimed();
    }
}

static void reset_stuck_returns_bounded_timeout_and_cleans_up(void) {
    struct reliefos_driver_kernel_api api;
    struct hda_controller c;
    fake_device_init(0x40, 0x40);
    fake.stuck_crst_clear = 1;
    fake.regs[0x08] = 1;
    api = fake_api();
    assert(hda_controller_init(&c, &api, &fake_pci) == -ETIMEDOUT);
    assert(fake.ticks <= 12);
    fake_assert_resources_reclaimed();
    assert(!fake.free_while_dma_live);

    fake_device_init(0x40, 0x40);
    fake.stuck_crst_set = 1;
    api = fake_api();
    assert(hda_controller_init(&c, &api, &fake_pci) == -ETIMEDOUT);
    assert(fake.ticks <= 12);
    fake_assert_resources_reclaimed();
    assert(!fake.free_while_dma_live);
}

static void unsolicited_and_other_cad_do_not_complete_the_flight(void) {
    struct reliefos_driver_kernel_api api;
    struct hda_controller c;
    struct hda_unsolicited_response event;
    uint64_t ticket;
    uint32_t response;
    fake_device_init(0x40, 0x40);
    fake.auto_response = 0;
    api = fake_api();
    assert(hda_controller_init(&c, &api, &fake_pci) == 0);
    assert(hda_verb_submit(&c, 2, 3, 0xf00, 0, false, &ticket) == 0);
    fake_push_response(2, 0xdead0001u, 1);
    fake_push_response(1, 0xdead0002u, 0);
    assert(hda_controller_service_budget(&c, 8) <= 8);
    assert(hda_verb_poll(&c, ticket, &response) == -EAGAIN);
    assert(hda_unsolicited_pop(&c, &event) == 0);
    assert(event.cad == 2 && event.response == 0xdead0001u);
    fake_push_response(2, 0xcafebabeu, 0);
    assert(hda_controller_service_budget(&c, 8) <= 8);
    assert(hda_verb_poll(&c, ticket, &response) == 0 && response == 0xcafebabeu);
    assert(hda_controller_destroy(&c) == 0);
    fake_assert_resources_reclaimed();
}

/* The real control service must leave another CAD's identical-tag event queued. */
static void controls_service_preserves_other_codec_jack_events(void) {
    fake_device_init(0x40, 0x40);
    fake.auto_response = 0u;
    struct reliefos_driver_kernel_api api = fake_api();
    struct hda_controller c;
    assert(hda_controller_init(&c, &api, &fake_pci) == 0);
    struct hda_codec codec1 = {.controller = &c, .cad = 1u};
    struct hda_codec codec2 = {.controller = &c, .cad = 2u};
    struct hda_controls controls1 = {.codec = &codec1, .headphone_tag = 3u,
                                    .headphone_pin = 5u};
    struct hda_controls controls2 = {.codec = &codec2, .headphone_tag = 3u,
                                    .headphone_pin = 5u};
    fake_push_response(2u, 3u << 26, 1u);
    (void)hda_controller_service_budget(&c, 8u);
    assert(c.unsolicited_count == 1u);
    assert(hda_controls_service(&controls1, 1u) == 0u);
    assert(c.unsolicited_count == 1u);
    assert(!controls1.sense_pending && !controls1.sense_inflight);
    assert(!c.flight.pending);
    assert(hda_controls_service(&controls2, 2u) == 2u);
    assert(c.unsolicited_count == 0u);
    assert(controls2.sense_inflight && c.flight.pending && c.flight.cad == 2u);
    assert(hda_controller_destroy(&c) == 0);
    fake_assert_resources_reclaimed();
}

/* Cross-CAD removal must preserve FIFO order even when the shared ring wraps. */
static void codec_event_pop_preserves_fifo_and_output_on_failure(void) {
    fake_device_init(0x40, 0x40);
    struct reliefos_driver_kernel_api api = fake_api();
    struct hda_controller c;
    assert(hda_controller_init(&c, &api, &fake_pci) == 0);
    c.unsolicited_head = HDA_UNSOLICITED_CAPACITY - 2u;
    const uint8_t cad[] = {1u, 2u, 1u, 3u, 2u, 15u};
    for (uint32_t i = 0u; i < 6u; ++i) fake_push_response(cad[i], 100u + i, 1u);
    assert(hda_controller_service_budget(&c, 8u) == 6u);
    struct hda_unsolicited_response out = {.cad = 14u, .response = 999u};
    assert(hda_unsolicited_pop_for_codec(&c, 4u, &out) == -EAGAIN);
    assert(out.cad == 14u && out.response == 999u && c.unsolicited_count == 6u);
    assert(hda_unsolicited_pop_for_codec(&c, 16u, &out) == -EINVAL);
    assert(hda_unsolicited_pop_for_codec(&c, 2u, &out) == 0);
    assert(out.cad == 2u && out.response == 101u);
    assert(hda_unsolicited_pop_for_codec(&c, 2u, &out) == 0);
    assert(out.cad == 2u && out.response == 104u);
    const uint32_t remaining[] = {100u, 102u, 103u, 105u};
    const uint8_t remaining_cad[] = {1u, 1u, 3u, 15u};
    for (uint32_t i = 0u; i < 4u; ++i) {
        assert(hda_unsolicited_pop(&c, &out) == 0);
        assert(out.response == remaining[i] && out.cad == remaining_cad[i]);
    }
    assert(hda_unsolicited_pop_for_codec(&c, 1u, &out) == -EAGAIN);
    assert(hda_controller_destroy(&c) == 0);
    assert(hda_unsolicited_pop_for_codec(&c, 1u, &out) == -ENODEV);
    assert(hda_unsolicited_pop_for_codec(NULL, 1u, &out) == -EINVAL);
    assert(hda_unsolicited_pop_for_codec(&c, 1u, NULL) == -EINVAL);
    fake_assert_resources_reclaimed();
}

/* A one-unit budget must defer queue consumption, then defer sense submission. */
static void controls_service_defers_work_when_budget_is_exhausted(void) {
    fake_device_init(0x40, 0x40);
    fake.auto_response = 0u;
    struct reliefos_driver_kernel_api api = fake_api();
    struct hda_controller c;
    assert(hda_controller_init(&c, &api, &fake_pci) == 0);
    struct hda_codec codec = {.controller = &c, .cad = 2u};
    struct hda_controls controls = {.codec = &codec, .headphone_tag = 3u,
                                   .headphone_pin = 5u};
    fake_push_response(2u, 3u << 26, 1u);
    assert(hda_controls_service(&controls, 1u) == 1u);
    assert(c.unsolicited_count == 1u && !c.flight.pending);
    assert(hda_controls_service(&controls, 1u) == 1u);
    assert(c.unsolicited_count == 0u && controls.sense_pending);
    assert(!controls.sense_inflight && !c.flight.pending);
    assert(hda_controls_service(&controls, 1u) == 1u);
    assert(controls.sense_inflight && c.flight.pending && c.flight.cad == 2u);
    fake_push_response(2u, HDA_PIN_SENSE_PRESENCE, 0u);
    assert(hda_controls_service(&controls, 1u) == 1u);
    assert(controls.sense_inflight && !controls.headphone_present);
    assert(hda_controls_service(&controls, 1u) == 1u);
    assert(!controls.sense_inflight && controls.headphone_present);
    assert(controls.jack_events == 1u);
    assert(hda_controller_destroy(&c) == 0);
    fake_assert_resources_reclaimed();
}

/* An event received after submission must survive completion of the old sense. */
static void controls_new_event_survives_old_sense_completion(void) {
    fake_device_init(0x40, 0x40);
    fake.auto_response = 0u;
    struct reliefos_driver_kernel_api api = fake_api();
    struct hda_controller c;
    assert(!hda_controller_init(&c, &api, &fake_pci));
    struct hda_codec codec = {.controller = &c, .cad = 2u};
    struct hda_controls controls = {.codec = &codec, .headphone_tag = 3u,
                                   .headphone_pin = 5u, .sense_pending = 1u};
    assert(hda_controls_service(&controls, 1u) == 1u);
    assert(controls.sense_inflight && !controls.sense_pending);
    fake_push_response(2u, 3u << 26, 1u);
    fake_push_response(2u, 0u, 0u);
    assert(hda_controller_service_budget(&c, 8u) == 2u);
    assert(hda_controls_service(&controls, 2u) == 2u);
    assert(!controls.sense_inflight && controls.sense_pending);
    assert(hda_controls_service(&controls, 1u) == 1u);
    assert(controls.sense_inflight && !controls.sense_pending);
    fake_push_response(2u, HDA_PIN_SENSE_PRESENCE, 0u);
    assert(hda_controller_service_budget(&c, 8u) == 1u);
    assert(hda_controls_service(&controls, 1u) == 1u);
    assert(controls.headphone_present && controls.jack_events == 1u);
    assert(!hda_controller_destroy(&c));
    fake_assert_resources_reclaimed();
    puts("jack_event_during_sense_requires_fresh_resample PASS");
}

static void controller_interrupt_acknowledges_real_sources_only(void) {
    struct reliefos_driver_kernel_api api;
    struct hda_controller c;
    uint64_t ticket;
    uint32_t response;
    fake_device_init(0x40, 0x40);
    fake.auto_response = 0u;
    fake.regs[0x0c] = 1u; /* Stale firmware wake enable survives link reset. */
    fake.regs[0x0e] = 1u; /* Matching stale STATESTS source. */
    api = fake_api();
    assert(hda_controller_init(&c, &api, &fake_pci) == 0);
    assert(hda_test_mmio_read8(&fake, HDA_REG_CORBCTL) & 1u);
    assert(hda_test_mmio_read16(&fake, 0x0c) == 0u);
    assert(hda_test_mmio_read16(&fake, 0x0e) == 0u);
    assert(hda_test_mmio_read32(&fake, 0x24) == 0u);
    assert((hda_test_mmio_read32(&fake, 0x20) & 0x7fffffffu) == (1u << 30));

    fake.regs[0x0c] = 1u;
    fake.regs[0x0e] = 1u;
    assert(hda_test_mmio_read32(&fake, 0x24) == 0xc0000000u);
    assert(fake.irq_handler);
    uint32_t intctl = hda_test_mmio_read32(&fake, HDA_REG_INTCTL);
    hda_test_mmio_write32(&fake, HDA_REG_INTCTL, intctl & ~(1u << 30));
    assert(hda_test_mmio_read32(&fake, 0x24) == 0xc0000000u);
    assert(!fake_irq_asserted());
    hda_test_mmio_write32(&fake, HDA_REG_INTCTL, intctl & ~(1u << 31));
    assert(hda_test_mmio_read32(&fake, 0x24) == 0xc0000000u);
    assert(!fake_irq_asserted());
    hda_test_mmio_write32(&fake, HDA_REG_INTCTL, intctl);
    assert(fake_irq_asserted());
    fake_deliver_irq();
    assert(hda_test_mmio_read16(&fake, 0x0e) == 0u);
    assert(hda_test_mmio_read32(&fake, 0x24) == 0u);

    assert(hda_verb_submit(&c, 1u, 2u, 0xf00u, 0u, false, &ticket) == 0);
    fake_push_response(1u, 0x31415926u, 0);
    assert(hda_test_mmio_read32(&fake, 0x24) == 0xc0000000u);
    assert(fake_irq_asserted());
    fake_deliver_irq();
    assert(hda_test_mmio_read32(&fake, 0x24) == 0u);
    assert(hda_verb_poll(&c, ticket, &response) == 0 && response == 0x31415926u);

    assert(hda_verb_submit(&c, 1u, 2u, 0xf00u, 0u, false, &ticket) == 0);
    fake.regs[HDA_REG_CORBSTS] |= 1u; /* CMEI is enabled by CORBCTL.CMEIE. */
    assert(hda_test_mmio_read32(&fake, HDA_REG_INTSTS) == 0xc0000000u);
    assert(fake_irq_asserted());
    fake_deliver_irq();
    assert(hda_test_mmio_read8(&fake, HDA_REG_CORBSTS) == 0u);
    assert(hda_test_mmio_read32(&fake, HDA_REG_INTSTS) == 0u);
    assert(hda_verb_poll(&c, ticket, &response) == -EIO);
    assert(hda_verb_submit(&c, 1u, 2u, 0xf00u, 0u, false, &ticket) == -EIO);
    for (uint32_t i = 0; i < 40u && c.recovery_state != HDA_RECOVERY_IDLE; ++i) {
        ++fake.ticks;
        (void)hda_controller_service_budget(&c, 1u);
    }
    assert(c.recovery_state == HDA_RECOVERY_IDLE && !c.transport_poisoned);
    assert(fake.reset_count == 1u);
    assert(hda_verb_submit(&c, 1u, 2u, 0xf00u, 0u, false, &ticket) == 0);
    assert(hda_controller_service_budget(&c, 1u) == 0u);
    assert(hda_verb_poll(&c, ticket, &response) == -EAGAIN);
    assert(hda_controller_destroy(&c) == 0);
    fake_assert_resources_reclaimed();
}

static void rirb_wrap_preserves_order_with_two_entry_ring(void) {
    struct reliefos_driver_kernel_api api;
    struct hda_controller c;
    struct hda_unsolicited_response event;
    fake_device_init(0x10, 0x10);
    api = fake_api();
    assert(hda_controller_init(&c, &api, &fake_pci) == 0);
    for (uint32_t i = 0; i < 7; ++i) {
        fake_push_response((uint8_t)(i & 1u), 0x100u + i, 1);
        assert(hda_controller_service_budget(&c, 1) <= 1);
        assert(hda_unsolicited_pop(&c, &event) == 0);
        assert(event.cad == (uint8_t)(i & 1u));
        assert(event.response == 0x100u + i);
    }
    assert(c.rirb_read_pointer == 1u);
    assert(hda_controller_destroy(&c) == 0);
    fake_assert_resources_reclaimed();
}

static void overrun_poison_requires_real_reset_before_same_cad_reuse(void) {
    struct reliefos_driver_kernel_api api;
    struct hda_controller c;
    uint64_t old_ticket, new_ticket;
    uint32_t response;
    fake_device_init(0x40, 0x40);
    fake.auto_response = 0;
    fake.defer_next_response = 1;
    api = fake_api();
    assert(hda_controller_init(&c, &api, &fake_pci) == 0);
    assert(hda_verb_submit(&c, 3, 4, 0xf00, 0, false, &old_ticket) == 0);
    fake.regs[0x5d] |= 4u;
    assert(hda_controller_service_budget(&c, 4) == 0);
    assert(hda_verb_poll(&c, old_ticket, &response) == -EOVERFLOW);
    assert(hda_verb_submit(&c, 3, 4, 0xf00, 0, false, &new_ticket) == -EIO);
    for (uint32_t i = 0; i < 16u && c.recovery_state != HDA_RECOVERY_IDLE; ++i) {
        fake.ticks += 2;
        hda_controller_service(&c);
    }
    assert(c.recovery_state == HDA_RECOVERY_IDLE);
    assert(fake.reset_count > 0 && fake.stale_discarded == 1);
    assert(hda_verb_submit(&c, 3, 4, 0xf00, 0, false, &new_ticket) == 0);
    assert(hda_controller_service_budget(&c, 4) == 0);
    assert(hda_verb_poll(&c, new_ticket, &response) == -EAGAIN);
    struct hda_unsolicited_response event;
    fake_push_response(3, 0xabcdef01u, 1);
    assert(hda_controller_service_budget(&c, 4) <= 4);
    assert(hda_unsolicited_pop(&c, &event) == 0);
    assert(event.cad == 3u && event.response == 0xabcdef01u);
    fake_push_response(3, 0x12345678u, 0);
    assert(hda_controller_service_budget(&c, 4) <= 4);
    assert(hda_verb_poll(&c, new_ticket, &response) == 0 && response == 0x12345678u);
    assert(hda_controller_destroy(&c) == 0);
    fake_assert_resources_reclaimed();
}

static void recovery_waits_for_stopped_rings_and_observed_crst(void) {
    struct reliefos_driver_kernel_api api;
    struct hda_controller c;
    uint64_t ticket;
    uint32_t response;
    fake_device_init(0x40, 0x40);
    fake.auto_response = 0u;
    api = fake_api();
    assert(hda_controller_init(&c, &api, &fake_pci) == 0);
    uint32_t reset_write_baseline = fake.ring_write_while_reset;
    fake.delay_corb_stop_ticks = 2u;
    fake.delay_rirb_stop_ticks = 3u;
    fake.delay_crst_low_ticks = 2u;
    fake.delay_crst_high_ticks = 3u;
    assert(hda_verb_submit(&c, 3u, 4u, 0xf00u, 0u, false, &ticket) == 0);
    fake.regs[0x5d] |= 4u;
    assert(hda_controller_service_budget(&c, 1u) == 0u);
    assert(fake.crst_low_while_dma_running == 0u);
    assert((hda_test_mmio_read32(&fake, 0x08) & 1u) != 0u);

    for (uint32_t i = 0; i < 40u && c.recovery_state != HDA_RECOVERY_IDLE &&
         c.recovery_state != HDA_RECOVERY_FAILED; ++i) {
        ++fake.ticks;
        hda_controller_service_budget(&c, 1u);
        if (!(hda_test_mmio_read32(&fake, 0x08) & 1u))
            assert(!(fake.regs[0x4c] & 2u) && !(fake.regs[0x5c] & 2u));
    }
    assert(c.recovery_state == HDA_RECOVERY_IDLE && !c.transport_poisoned);
    assert(fake.crst_low_while_dma_running == 0u);
    assert(fake.ring_write_while_reset == reset_write_baseline);
    assert(fake.ring_write_before_stable == 0u);
    assert(fake.crst_high_tick != 0u && fake.reset_count == 1u);
    assert(hda_test_mmio_read32(&fake, 0x08) & 1u);
    assert(hda_test_mmio_read32(&fake, 0x08) & (1u << 8));
    assert_ring_register_publication(&c);
    assert(hda_verb_poll(&c, ticket, &response) == -EOVERFLOW);
    assert(hda_controller_destroy(&c) == 0);
    fake_assert_resources_reclaimed();
}

static void recovery_stuck_stop_and_crst_release_keep_poison_and_leases(void) {
    struct reliefos_driver_kernel_api api;
    struct hda_controller c;
    uint64_t ticket;
    uint32_t response;

    fake_device_init(0x40, 0x40);
    fake.auto_response = 0u;
    api = fake_api();
    assert(hda_controller_init(&c, &api, &fake_pci) == 0);
    assert(hda_controller_set_card_id(&c,0x11223300u)==0);
    assert(hda_verb_submit(&c, 1u, 2u, 0xf00u, 0u, false, &ticket) == 0);
    fake.stuck_rirb_stop = 1u;
    fake.regs[0x5d] |= 4u;
    assert(hda_controller_service_budget(&c, 1u) == 0u);
    for (uint32_t i = 0; i < 24u && c.recovery_state != HDA_RECOVERY_FAILED; ++i) {
        ++fake.ticks;
        (void)hda_controller_service_budget(&c, 1u);
    }
    assert(c.recovery_state == HDA_RECOVERY_FAILED && c.transport_poisoned);
    assert(c.recovery_error == -ETIMEDOUT);
    assert(hda_test_mmio_read32(&fake, 0x08) & 1u);
    assert(!(fake.regs[0x4c] & 2u) && (fake.regs[0x5c] & 2u));
    assert(fake.reset_count == 0u);
    assert(fake.live_dma == 2u && fake.live_maps == 1u);
    uint32_t card=0;
    assert(hda_controller_take_disconnect_request(&c,&card)==0 && card==0x11223300u);
    assert(hda_verb_poll(&c, ticket, &response) == -EOVERFLOW);
    fake.stuck_rirb_stop = 0u;
    assert(hda_controller_destroy(&c) == 0);
    fake_assert_resources_reclaimed();

    fake_device_init(0x40, 0x40);
    fake.auto_response = 0u;
    api = fake_api();
    assert(hda_controller_init(&c, &api, &fake_pci) == 0);
    uint32_t ring_writes_before_recovery = fake.ring_write_while_reset;
    assert(hda_verb_submit(&c, 1u, 2u, 0xf00u, 0u, false, &ticket) == 0);
    fake.stuck_crst_set = 1u;
    fake.regs[0x5d] |= 4u;
    assert(hda_controller_service_budget(&c, 1u) == 0u);
    for (uint32_t i = 0; i < 32u && c.recovery_state != HDA_RECOVERY_FAILED; ++i) {
        ++fake.ticks;
        (void)hda_controller_service_budget(&c, 1u);
    }
    assert(c.recovery_state == HDA_RECOVERY_FAILED && c.transport_poisoned);
    assert(c.recovery_error == -ETIMEDOUT);
    assert(!(hda_test_mmio_read32(&fake, 0x08) & 1u));
    assert(fake.ring_write_while_reset == ring_writes_before_recovery);
    assert(fake.reset_count == 1u);
    assert(fake.live_dma == 2u && fake.live_maps == 1u);
    assert(hda_verb_poll(&c, ticket, &response) == -EOVERFLOW);
    fake.stuck_crst_set = 0u;
    assert(hda_controller_destroy(&c) == 0);
    fake_assert_resources_reclaimed();
}

static void response_seen_after_deadline_cannot_win_the_ticket(void) {
    struct reliefos_driver_kernel_api api;
    struct hda_controller c;
    uint64_t ticket, next_ticket;
    uint32_t response;
    fake_device_init(0x40, 0x40);
    fake.auto_response = 0;
    api = fake_api();
    assert(hda_controller_init(&c, &api, &fake_pci) == 0);
    assert(hda_verb_submit(&c, 4, 2, 0xf00, 0, false, &ticket) == 0);
    fake.ticks += 10u;
    fake_push_response(4, 0xfeedfaceu, 0);
    assert(hda_controller_service(&c) <= HDA_SERVICE_BUDGET);
    assert(hda_verb_poll(&c, ticket, &response) == -ETIMEDOUT);
    assert(c.transport_mode == HDA_TRANSPORT_RINGS);
    assert(hda_verb_submit(&c, 4, 2, 0xf00, 0, false, &next_ticket) == -EIO);
    for (uint32_t i = 0; i < 16u && c.recovery_state != HDA_RECOVERY_IDLE; ++i) {
        fake.ticks += 2u;
        hda_controller_service(&c);
    }
    assert(!c.transport_poisoned && fake.reset_count > 0u);
    assert(c.transport_mode == HDA_TRANSPORT_RINGS);
    assert(hda_controller_destroy(&c) == 0);
    fake_assert_resources_reclaimed();
}

static void ring_handshake_failure_is_bounded_and_reclaimed(void) {
    struct reliefos_driver_kernel_api api;
    struct hda_controller c;
    fake_device_init(0x40, 0x40);
    fake.stuck_corb_reset = 1;
    api = fake_api();
    assert(hda_controller_init(&c, &api, &fake_pci) == 0);
    assert(c.transport_mode == HDA_TRANSPORT_IMMEDIATE && !c.rings_started);
    assert(fake.ticks <= 4u * HDA_RESET_TIMEOUT_TICKS + 5u);
    assert(fake.live_dma == 0u && fake.live_maps == 1u && fake.live_irqs == 0u);
    assert(hda_controller_destroy(&c) == 0);
    fake_assert_resources_reclaimed();

    fake_device_init(0x40, 0x40);
    fake.stuck_rirb_wp_reset = 1;
    fake.regs[0x58] = 1u;
    api = fake_api();
    assert(hda_controller_init(&c, &api, &fake_pci) == 0);
    assert(c.transport_mode == HDA_TRANSPORT_IMMEDIATE && !c.rings_started);
    assert(hda_controller_destroy(&c) == 0);
    fake_assert_resources_reclaimed();

    for (uint32_t fail_rirb = 0; fail_rirb < 2u; ++fail_rirb) {
        fake_device_init(0x40, 0x40);
        fake.fail_rirb_start = fail_rirb;
        fake.fail_corb_start = !fail_rirb;
        api = fake_api();
        assert(hda_controller_init(&c, &api, &fake_pci) == 0);
        assert(c.transport_mode == HDA_TRANSPORT_IMMEDIATE && !c.rings_started);
        assert(fake.live_dma == 0u && fake.live_maps == 1u && fake.live_irqs == 0u);
        assert(hda_controller_destroy(&c) == 0);
        fake_assert_resources_reclaimed();
    }

    fake_device_init(0x40, 0x40);
    fake.fail_corb_start = 1u;
    api = fake_api();
    api.console_write = NULL;
    assert(hda_controller_init(&c, &api, &fake_pci) == -EOPNOTSUPP);
    fake_assert_resources_reclaimed();
}

static void immediate_fallback_keeps_pcm_dma_enabled(void) {
    for (unsigned failure = 0; failure < 2u; ++failure) {
        fake_device_init(0x40, 0x40);
        /* Preserve unrelated command bits and the read-only/status half. */
        const uint32_t original = 0x12340402u;
        fake.pci[1] = original;
        fake.fail_corb_start = failure == 0u;
        fake.fail_rirb_start = failure == 1u;
        struct reliefos_driver_kernel_api api = fake_api();
        struct hda_controller c;
        assert(!hda_controller_init(&c, &api, &fake_pci));
        assert(c.transport_mode == HDA_TRANSPORT_IMMEDIATE && !c.rings_started);
        assert(fake.pci[1] == (original | HDA_PCI_COMMAND_MASTER));
        assert(!(fake.regs[HDA_REG_CORBCTL] & HDA_CORB_RUN));
        assert(!(fake.regs[HDA_REG_RIRBCTL] & HDA_RIRB_DMA_ENABLE));
        assert(fake.live_dma == 0u && fake.live_irqs == 0u);
        for (unsigned reopen = 0; reopen < 2u; ++reopen) {
            assert(!hda_controller_stream_acquire(&c));
            assert(fake.pci[1] & HDA_PCI_COMMAND_MASTER);
            assert(!hda_controller_stream_release(&c));
        }
        assert(!hda_controller_destroy(&c));
        assert(fake.pci[1] == original);
        fake_assert_resources_reclaimed();
    }

    fake_device_init(0x40, 0x40);
    fake.pci[1] = HDA_PCI_COMMAND_MEMORY;
    fake.fail_corb_start = 1u;
    fake.fail_bus_master_reenable = 1u;
    struct reliefos_driver_kernel_api api = fake_api();
    struct hda_controller c;
    assert(hda_controller_init(&c, &api, &fake_pci) == -EIO);
    assert(!c.initialized);
    assert(fake.pci[1] == HDA_PCI_COMMAND_MEMORY);
    fake_assert_resources_reclaimed();
}

static void immediate_fallback_preserves_nonblocking_singleflight(void) {
    struct reliefos_driver_kernel_api api;
    struct hda_controller c;
    uint64_t ticket;
    uint32_t response;
    for (uint32_t version = 0u; version <= 1u; ++version) {
        fake_device_init(0x40, 0x40);
        fake.fail_corb_start = 1u;
        fake.immediate_version = version;
        api = fake_api();
        assert(hda_controller_init(&c, &api, &fake_pci) == 0);
        assert(c.transport_mode == HDA_TRANSPORT_IMMEDIATE);
        assert(c.immediate_version == version && !c.rings_started);
        assert(!c.owns_corb && !c.owns_rirb && !c.owns_irq && c.owns_mmio);
        assert(fake.live_dma == 0u && fake.live_irqs == 0u && fake.live_maps == 1u);
        assert(!(hda_test_mmio_read32(&fake, HDA_REG_GCTL) & HDA_GCTL_UNSOL));
        assert(fake.console_calls &&
               !strncmp(fake.last_log, "[hda] ring init failed; using immediate", 39u));
        uint32_t sleeps_before = fake.sleep_calls;
        assert(hda_verb_submit(&c, 2u, 3u, 0xf00u, 0u, false, &ticket) == 0);
        assert(hda_verb_poll(&c, ticket, &response) == -EAGAIN);
        assert(hda_controller_service_budget(&c, 1u) == 0u);
        assert(hda_verb_poll(&c, ticket, &response) == 0);
        assert(response == fake.response_value);
        assert(fake.sleep_calls == sleeps_before);
        assert(fake.immediate_command_count == 1u && fake.immediate_with_rings == 0u);
        struct hda_unsolicited_response event;
        assert(hda_unsolicited_pop(&c, &event) == -EAGAIN);
        assert(hda_controller_destroy(&c) == 0);
        fake_assert_resources_reclaimed();
    }
}

static void immediate_submit_checks_busy_and_clears_stale_valid(void) {
    struct reliefos_driver_kernel_api api;
    struct hda_controller c;
    uint64_t ticket;
    uint32_t response;
    fake_device_init(0x40, 0x40);
    fake.fail_rirb_start = 1u;
    api = fake_api();
    assert(hda_controller_init(&c, &api, &fake_pci) == 0);
    fake_store_immediate_status((uint16_t)(HDA_ICIS_ICVER | HDA_ICIS_ICB));
    uint32_t commands_before = fake.immediate_command_count;
    assert(hda_verb_submit(&c, 1u, 1u, 0xf00u, 0u, false, &ticket) == -EBUSY);
    assert(fake.immediate_command_count == commands_before);

    fake.immediate_auto_response = 0u;
    fake_store_immediate_status((uint16_t)(HDA_ICIS_ICVER | HDA_ICIS_IRV));
    assert(hda_verb_submit(&c, 1u, 1u, 0xf00u, 0u, false, &ticket) == 0);
    assert(fake.immediate_command_count == commands_before + 1u);
    assert(!(hda_test_mmio_read16(&fake, HDA_REG_ICIS) & HDA_ICIS_IRV));
    fake_complete_immediate(&fake, 1u, fake.response_value, 0);
    assert(hda_controller_service_budget(&c, 1u) == 0u);
    assert(hda_verb_poll(&c, ticket, &response) == 0);
    assert(response == fake.response_value);

    fake.immediate_stuck_valid = 1u;
    fake_store_immediate_status((uint16_t)(HDA_ICIS_ICVER | HDA_ICIS_IRV));
    commands_before = fake.immediate_command_count;
    assert(hda_verb_submit(&c, 1u, 1u, 0xf00u, 0u, false, &ticket) == -EIO);
    assert(fake.immediate_command_count == commands_before);
    fake.immediate_stuck_valid = 0u;
    assert(hda_controller_destroy(&c) == 0);
    fake_assert_resources_reclaimed();
}

static void immediate_busy_submit_preserves_owner_response(void) {
    struct reliefos_driver_kernel_api api;
    struct hda_controller c;
    for (uint32_t version = 0u; version <= 1u; ++version) {
        fake_device_init(0x40, 0x40);
        fake.fail_corb_start = 1u;
        fake.immediate_version = version;
        api = fake_api();
        assert(hda_controller_init(&c, &api, &fake_pci) == 0);
        assert(c.transport_mode == HDA_TRANSPORT_IMMEDIATE);

        uint64_t owner_ticket;
        uint64_t contender_ticket = UINT64_C(0xfeedfacecafebeef);
        assert(hda_verb_submit(&c, 3u, 4u, 0xf00u, 0x1234u, false,
                               &owner_ticket) == 0);
        uint16_t status_before = hda_test_mmio_read16(&fake, HDA_REG_ICIS);
        uint32_t response_before = hda_test_mmio_read32(&fake, HDA_REG_ICII);
        uint32_t command_before = hda_test_mmio_read32(&fake, HDA_REG_ICOI);
        uint64_t next_ticket_before = c.next_ticket;
        uint64_t owner_start_before = c.flight.start_tick;
        uint32_t command_count_before = fake.immediate_command_count;
        assert((status_before & (HDA_ICIS_IRV | HDA_ICIS_ICB)) == HDA_ICIS_IRV);
        assert(response_before == fake.response_value);
        assert(command_before == hda_encode_verb(3u, 4u, 0xf00u, 0x1234u, false));
        assert(c.flight.ticket == owner_ticket && c.flight.pending && !c.flight.complete);

        assert(hda_verb_submit(&c, 5u, 6u, 0xf00u, 0u, false,
                               &contender_ticket) == -EBUSY);
        assert(hda_test_mmio_read16(&fake, HDA_REG_ICIS) == status_before);
        assert(hda_test_mmio_read32(&fake, HDA_REG_ICII) == response_before);
        assert(hda_test_mmio_read32(&fake, HDA_REG_ICOI) == command_before);
        assert(c.next_ticket == next_ticket_before);
        assert(c.flight.ticket == owner_ticket && c.flight.start_tick == owner_start_before);
        assert(c.flight.pending && !c.flight.complete && c.flight.error == 0);
        assert(c.flight.response == 0u && c.flight.cad == 3u);
        assert(fake.immediate_command_count == command_count_before);
        assert(contender_ticket == UINT64_C(0xfeedfacecafebeef));

        assert(hda_controller_service_budget(&c, 1u) == 0u);
        uint32_t response = 0u;
        assert(hda_verb_poll(&c, owner_ticket, &response) == 0);
        assert(response == response_before);
        assert(hda_verb_poll(&c, owner_ticket, &response) == -ENOENT);
        assert(hda_controller_destroy(&c) == 0);
        fake_assert_resources_reclaimed();
    }
}

static void immediate_version_one_rejects_wrong_codec_response(void) {
    struct reliefos_driver_kernel_api api;
    struct hda_controller c;
    uint64_t ticket;
    uint32_t response;
    fake_device_init(0x40, 0x40);
    fake.fail_corb_start = 1u;
    fake.immediate_override = 1u;
    fake.immediate_override_cad = 2u;
    fake.immediate_override_response = 0xdead1234u;
    api = fake_api();
    assert(hda_controller_init(&c, &api, &fake_pci) == 0);
    assert(c.immediate_version == 1u);
    assert(hda_verb_submit(&c, 1u, 2u, 0xf00u, 0u, false, &ticket) == 0);
    assert(hda_controller_service_budget(&c, 1u) == 0u);
    assert(hda_verb_poll(&c, ticket, &response) == -EAGAIN);
    assert(c.flight.pending && c.unexpected_responses == 1u);
    fake.ticks += 10u;
    (void)hda_controller_service_budget(&c, 1u);
    assert(hda_verb_poll(&c, ticket, &response) == -ETIMEDOUT);
    uint32_t commands_before = fake.immediate_command_count;
    assert(hda_verb_submit(&c, 1u, 2u, 0xf00u, 0u, false, &ticket) == -EIO);
    assert(fake.immediate_command_count == commands_before);
    assert(hda_controller_recover(&c) == 0);
    assert(c.transport_mode == HDA_TRANSPORT_IMMEDIATE && !c.transport_poisoned);
    fake.immediate_override = 0u;
    assert(hda_verb_submit(&c, 1u, 2u, 0xf00u, 0u, false, &ticket) == 0);
    assert(hda_controller_service_budget(&c, 1u) == 0u);
    assert(hda_verb_poll(&c, ticket, &response) == 0 && response == fake.response_value);
    assert(hda_controller_destroy(&c) == 0);
    fake_assert_resources_reclaimed();
}

static void immediate_no_progress_times_out_until_real_reset_flush(void) {
    struct reliefos_driver_kernel_api api;
    struct hda_controller c;
    uint64_t ticket;
    uint32_t response;
    fake_device_init(0x40, 0x40);
    fake.fail_corb_start = 1u;
    fake.immediate_defer_response = 1u;
    api = fake_api();
    assert(hda_controller_init(&c, &api, &fake_pci) == 0);
    assert(hda_verb_submit(&c, 4u, 2u, 0xf00u, 0u, false, &ticket) == 0);
    assert(fake.immediate_late_pending && fake.immediate_late_cad == 4u);
    assert(fake.immediate_late_response == 0x0badc0deu);
    uint32_t commands_before = fake.immediate_command_count;
    fake.ticks += 10u;
    (void)hda_controller_service_budget(&c, 1u);
    assert(hda_verb_poll(&c, ticket, &response) == -ETIMEDOUT);
    assert(fake.immediate_command_count == commands_before);
    assert(hda_verb_submit(&c, 4u, 2u, 0xf00u, 0u, false, &ticket) == -EIO);
    assert(fake.immediate_reset_discarded == 0u);
    assert(hda_controller_recover(&c) == 0);
    assert(fake.immediate_reset_discarded == 1u);
    assert(!fake.immediate_late_pending);
    assert(!(hda_test_mmio_read16(&fake, HDA_REG_ICIS) &
             (HDA_ICIS_ICB | HDA_ICIS_IRV)));
    assert(c.transport_mode == HDA_TRANSPORT_IMMEDIATE && !c.transport_poisoned);
    assert(hda_verb_submit(&c, 4u, 2u, 0xf00u, 0u, false, &ticket) == 0);
    assert(hda_controller_service_budget(&c, 1u) == 0u);
    assert(hda_verb_poll(&c, ticket, &response) == 0 && response == fake.response_value);
    assert(hda_controller_destroy(&c) == 0);
    fake_assert_resources_reclaimed();

    fake_device_init(0x40, 0x40);
    fake.fail_rirb_start = 1u;
    fake.immediate_present = 0u; /* Optional PIO registers are not implemented. */
    fake.immediate_version = 0u;
    api = fake_api();
    assert(hda_controller_init(&c, &api, &fake_pci) == 0);
    assert(c.transport_mode == HDA_TRANSPORT_IMMEDIATE && c.immediate_version == 0u);
    assert(hda_verb_submit(&c, 5u, 1u, 0xf00u, 0u, false, &ticket) == 0);
    fake.ticks += 10u;
    (void)hda_controller_service_budget(&c, 1u);
    assert(hda_verb_poll(&c, ticket, &response) == -ETIMEDOUT);
    assert(hda_controller_recover(&c) == 0);
    assert(c.transport_mode == HDA_TRANSPORT_IMMEDIATE && !c.transport_poisoned);
    assert(fake.immediate_command_count == 0u);
    assert(hda_controller_destroy(&c) == 0);
    fake_assert_resources_reclaimed();
}

static void completed_ticket_is_consumed_once_and_errors_are_retained(void) {
    struct reliefos_driver_kernel_api api;
    struct hda_controller c;
    uint64_t ticket;
    uint32_t response;
    fake_device_init(0x40, 0x40);
    api = fake_api();
    assert(hda_controller_init(&c, &api, &fake_pci) == 0);
    assert(hda_verb_submit(&c, 1, 2, 0xf00, 0, false, &ticket) == 0);
    assert(hda_controller_service(&c) == 1u);
    assert(hda_verb_poll(&c, ticket + 1u, &response) == -ENOENT);
    assert(hda_verb_poll(&c, ticket, &response) == 0);
    assert(response == fake.response_value);
    assert(hda_verb_poll(&c, ticket, &response) == -ENOENT);
    assert(hda_controller_destroy(&c) == 0);
    fake_assert_resources_reclaimed();
}

static void repeated_recovered_failures_request_card_disconnect(void) {
    struct reliefos_driver_kernel_api api;
    struct hda_controller c;
    uint32_t response = 0u;
    uint32_t card = 0u;
    fake_device_init(0x40, 0x40);
    fake.auto_response = 0;
    api = fake_api();
    assert(hda_controller_init(&c, &api, &fake_pci) == 0);
    assert(hda_controller_set_card_id(&c, 0x10203040u) == 0);
    for (uint32_t i = 0; i < 3u; ++i)
        assert(hda_exec_verb(&c, 2, 1, 0xf00, 0, false, &response) == -ETIMEDOUT);
    assert(hda_controller_take_disconnect_request(&c, &card) == 0);
    assert(card == 0x10203040u);
    assert(hda_controller_take_disconnect_request(&c, &card) == -EAGAIN);
    assert(hda_controller_destroy(&c) == 0);
    fake_assert_resources_reclaimed();
}

static void ticket_sequence_never_wraps_or_reuses(void) {
    struct reliefos_driver_kernel_api api;
    struct hda_controller c;
    uint64_t ticket;
    uint32_t response;
    fake_device_init(0x40, 0x40);
    api = fake_api();
    assert(hda_controller_init(&c, &api, &fake_pci) == 0);
    c.next_ticket = UINT64_MAX;
    assert(hda_verb_submit(&c, 0, 1, 0xf00, 0, false, &ticket) == 0);
    assert(ticket == UINT64_MAX && c.next_ticket == 0u);
    assert(hda_controller_service(&c) == 1u);
    assert(hda_verb_poll(&c, ticket, &response) == 0);
    assert(hda_verb_submit(&c, 0, 1, 0xf00, 0, false, &ticket) == -ENOSPC);
    assert(hda_controller_destroy(&c) == 0);
    fake_assert_resources_reclaimed();
}

static void destroy_preserves_dma_leases_until_hardware_is_quiet(void) {
    struct reliefos_driver_kernel_api api;
    struct hda_controller c;
    fake_device_init(0x20, 0x20);
    api = fake_api();
    assert(hda_controller_init(&c, &api, &fake_pci) == 0);
    fake.stuck_rirb_stop = 1;
    fake.stuck_crst_clear = 1;
    assert(hda_controller_destroy(&c) == -ETIMEDOUT);
    assert(fake.live_dma == 2 && fake.live_maps == 1 && fake.live_irqs == 1);
    assert(!fake.free_while_dma_live);
    fake.stuck_rirb_stop = 0;
    fake.stuck_crst_clear = 0;
    assert(hda_controller_destroy(&c) == 0);
    fake_assert_resources_reclaimed();
}

static void failed_init_retained_leases_stay_nonserviceable_until_destroy(void) {
    struct reliefos_driver_kernel_api api;
    struct hda_controller c;
    fake_device_init(0x40, 0x40);
    fake.fail_corb_start = 1u;
    fake.stuck_rirb_stop = 1u;
    fake.stuck_crst_clear = 1u;
    api = fake_api();
    assert(hda_controller_init(&c, &api, &fake_pci) == -ETIMEDOUT);
    assert(!c.initialized && c.owns_mmio && c.owns_corb && c.owns_rirb && c.owns_irq);
    assert(c.transport_mode == HDA_TRANSPORT_UNAVAILABLE && fake.console_calls == 0u);
    assert(fake.live_dma == 2u && fake.live_maps == 1u && fake.live_irqs == 1u);
    assert(hda_controller_service(&c) == 0u);
    assert(hda_controller_destroy(&c) == -ETIMEDOUT);
    assert(!c.initialized);
    assert(fake.live_dma == 2u && fake.live_maps == 1u && fake.live_irqs == 1u);
    fake.stuck_rirb_stop = 0u;
    fake.stuck_crst_clear = 0u;
    assert(hda_controller_destroy(&c) == 0);
    fake_assert_resources_reclaimed();
}

static void active_stream_blocks_transparent_recovery_until_task_recovery(void) {
    struct reliefos_driver_kernel_api api;
    struct hda_controller c;
    uint64_t ticket;
    uint32_t response;
    fake_device_init(0x40, 0x40);
    fake.auto_response = 0;
    api = fake_api();
    assert(hda_controller_init(&c, &api, &fake_pci) == 0);
    assert(hda_controller_set_card_id(&c,0x10203040u)==0);
    assert(hda_controller_stream_acquire(&c) == 0);
    assert(hda_verb_submit(&c, 0, 1, 0xf00, 0, false, &ticket) == 0);
    fake.regs[0x5d] |= 4u;
    hda_controller_service(&c);
    assert(hda_verb_poll(&c, ticket, &response) == -EOVERFLOW);
    fake.ticks += 40;
    hda_controller_service(&c);
    assert(fake.reset_count == 0);
    uint32_t card=0;
    assert(hda_controller_take_disconnect_request(&c,&card)==0 && card==0x10203040u);
    assert(c.active_streams==1 && c.transport_poisoned && fake.live_dma);
    assert(hda_controller_recover(&c) == -EBUSY);
    assert(hda_controller_stream_release(&c) == 0);
    assert(hda_controller_set_recovery_allowed(&c, true) == 0);
    assert(hda_controller_recover(&c) == 0);
    assert(fake.reset_count == 1);
    assert(hda_controller_destroy(&c) == 0);
    fake_assert_resources_reclaimed();
}

static void failed_msi_uses_bounded_polling_transport(void) {
    struct reliefos_driver_kernel_api api;
    struct hda_controller c;
    uint32_t response;
    fake_device_init(0x20, 0x20);
    fake.fail_irq = 1;
    fake.qemu_response_limit = 1u;
    api = fake_api();
    assert(hda_controller_init(&c, &api, &fake_pci) == 0);
    assert(!c.irq_handle && !fake.live_irqs);
    assert(!(hda_test_mmio_read8(&fake, HDA_REG_CORBCTL) & HDA_CORB_MEMORY_ERROR_IRQ));
    assert(hda_test_mmio_read8(&fake, HDA_REG_RIRBCTL) & HDA_RIRB_RESPONSE_IRQ);
    assert(!(hda_test_mmio_read8(&fake, HDA_REG_RIRBCTL) & HDA_RIRB_OVERRUN_IRQ));
    assert(!(hda_test_mmio_read32(&fake, HDA_REG_INTCTL) & 0xc0000000u));
    assert(hda_exec_verb(&c, 0, 1, 0xf00, 0, false, &response) == 0);
    assert(response == fake.response_value);
    assert(hda_exec_verb(&c, 0, 1, 0xf00, 2, false, &response) == 0);
    assert(!(hda_test_mmio_read8(&fake, HDA_REG_RIRBSTS) & HDA_RIRB_STATUS_RESPONSE));
    assert(hda_controller_destroy(&c) == 0);
    fake_assert_resources_reclaimed();
}

int main(void) {
    verb_layout_is_correct();
    sleep_preserves_if_state_and_allows_interrupt_progress();
    ring_sizes_follow_capabilities();
    reset_stuck_returns_bounded_timeout_and_cleans_up();
    rirb_wrap_preserves_order_with_two_entry_ring();
    unsolicited_and_other_cad_do_not_complete_the_flight();
    controls_service_preserves_other_codec_jack_events();
    codec_event_pop_preserves_fifo_and_output_on_failure();
    controls_service_defers_work_when_budget_is_exhausted();
    controls_new_event_survives_old_sense_completion();
    controller_interrupt_acknowledges_real_sources_only();
    overrun_poison_requires_real_reset_before_same_cad_reuse();
    recovery_waits_for_stopped_rings_and_observed_crst();
    recovery_stuck_stop_and_crst_release_keep_poison_and_leases();
    response_seen_after_deadline_cannot_win_the_ticket();
    active_stream_blocks_transparent_recovery_until_task_recovery();
    failed_msi_uses_bounded_polling_transport();
    ring_handshake_failure_is_bounded_and_reclaimed();
    completed_ticket_is_consumed_once_and_errors_are_retained();
    repeated_recovered_failures_request_card_disconnect();
    ticket_sequence_never_wraps_or_reuses();
    immediate_fallback_keeps_pcm_dma_enabled();
    immediate_fallback_preserves_nonblocking_singleflight();
    immediate_submit_checks_busy_and_clears_stale_valid();
    immediate_busy_submit_preserves_owner_response();
    immediate_version_one_rejects_wrong_codec_response();
    immediate_no_progress_times_out_until_real_reset_flush();
    destroy_preserves_dma_leases_until_hardware_is_quiet();
    failed_init_retained_leases_stay_nonserviceable_until_destroy();
    reset_failure_releases_every_acquired_lease();
    assert(hda_total_sleep_under_lock == 0u);
    puts("PASS HDA controller, reset, rings and codec transport");
    return 0;
}
