/* Execute the real inventory parsers and namespace against hardware fixtures. */
#define TEST_REAL_INVENTORY
#include <reliefnt/console.h>
#include "../../kernel/reliefnt/arch/x86_64/cpu/cpuinfo.c"
#include "../../kernel/reliefnt/arch/x86_64/platform/platform.c"
#include "../../kernel/reliefnt/fs/sysfs/sysfs.c"
#define main proc_fixture_main
#include "procfs_directories_test.c"
#undef main
#include <stdarg.h>
#include <stdlib.h>
#include <linux/errno.h>

static unsigned fixture_cpus=1;
static bool fixture_drm_present;
static bool fixture_render_present;
static bool fixture_kms_enabled;
int storage_install_list_disks(struct reliefos_install_disk *disks, uint32_t capacity, uint32_t *count)
{
    *count = 1;
    if (capacity) *disks = (struct reliefos_install_disk){.id=0, .sector_size=512, .sector_count=131072};
    return 0;
}
int storage_disk_block_info(uint32_t disk, int32_t part, uint64_t *start, uint64_t *count)
{
    if (disk || part > 0) return -2;
    *start = part < 0 ? 0 : 2048;
    *count = part < 0 ? 131072 : 32768;
    return 0;
}
/* This fixture has partition geometry but no filesystem sector contents. */
int storage_disk_block_read(uint32_t disk, int32_t part, uint64_t offset,
                           void *buffer, uint32_t length, uint32_t *got)
{
    (void)disk; (void)part; (void)offset; (void)buffer; (void)length;
    *got = 0;
    return -2;
}
/* sysfs.c is compiled as a standalone host fixture, so provide the same
 * deterministic spelling that the real storage mount module exports. */
int storage_disk_device_name(uint32_t disk, int32_t part, char *out, uint32_t capacity)
{
    int written;
    if (!out || capacity == 0 || disk != 0 || part < -1 || part >= 128) return -2;
    written = snprintf(out, capacity, "/dev/sda%s", part < 0 ? "" : "1");
    return written < 0 || (uint32_t)written >= capacity ? -22 : 0;
}
uint32_t smp_cpu_count(void) { return fixture_cpus; }
bool smp_cpu_online(uint32_t cpu)
{
    return cpu == 0 || (fixture_cpus > 2 && cpu == 2);
}
void console_printf(const char *format, ...)
{
    (void)format;
}

void console_log(enum console_log_level level, const char *format, ...)
{
    (void)level;
    (void)format;

}
static struct framebuffer fixture_fb = {
    .available = true, .width = 1280, .height = 720, .pitch = 5120, .bpp = 32};
const struct framebuffer *framebuffer_get(void)
{
    return &fixture_fb;
}
int pci_read_device(uint8_t bus, uint8_t slot, uint8_t fn, struct pci_device *out)
{
    if (bus || slot != 2 || fn)
        return -1;
    *out = (struct pci_device){.bus = bus,
                               .slot = slot,
                               .function = fn,
                               .vendor_id = 0x1234,
                               .device_id = 0x1111,
                               .class_code = 3};
    return 0;
}
uint32_t pci_config_read32(uint8_t b, uint8_t s, uint8_t f, uint8_t o)
{
    assert(!b && s == 2 && !f);
    return o == 0x2c ? 0x11001af4 : 0;
}
int drm_device_pci_identity(struct pci_device *out)
{
    if (!fixture_drm_present) return -LINUX_ENODEV;
    *out = (struct pci_device){.bus = 0, .slot = 2, .function = 0,
                               .vendor_id = 0x1234, .device_id = 0x1111,
                               .class_code = 3};
    return 0;
}
bool drm_device_render_node_ready(void)
{
    return fixture_drm_present && fixture_render_present;
}
bool drm_device_kms_scanout_active(void)
{
    return fixture_kms_enabled;
}
static void read_value(const char *path, char *out, size_t capacity)
{
    uint32_t total = 0, got;
    do {
        int ret = proc_read(path, total, out + total,
                            capacity - total > 7 ? 7 : capacity - total, &got);
        if (ret != 0) {
            fprintf(stderr, "proc_read failed path=%s offset=%u ret=%d\n",
                    path, total, ret);
            abort();
        }
        total += got;
    } while (got && total + 1 < capacity);
    out[total] = 0;
}
static void walk(const char *path, unsigned depth)
{
    assert(depth < 12);
    uint64_t offset = 0;
    struct reliefos_dir_entry entry;
    int ret;
    while ((ret = proc_readdir(path, &offset, &entry)) > 0) {
        char child[256], value[16384];
        struct storage_node node;
        snprintf(child, sizeof(child), "%s/%s", path, entry.name);
        assert(proc_lookup(child, &node) == 0 && node.type == entry.type);
        if (node.type == RELIEFOS_FS_TYPE_DIR)
            walk(child, depth + 1);
        else if (node.type == RELIEFOS_FS_TYPE_FILE)
            read_value(child, value, sizeof(value));
        else
            assert(proc_readlink(child, value, sizeof(value)) > 0);
    }
    assert(ret == 0);
}
int main(void)
{
    cpu_inventory_capture(0);
    char value[16384];
    struct storage_node node;
    assert(proc_lookup("/sys/dev/block", &node) == 0 && node.type == RELIEFOS_FS_TYPE_DIR);
    read_value("/sys/devices/platform/leonos-block/sda/dev", value, sizeof(value));
    assert(!strcmp(value, "259:0\n"));
    read_value("/sys/devices/platform/leonos-block/sda/sda1/partition", value, sizeof(value));
    assert(!strcmp(value, "1\n"));
    read_value("/sys/devices/platform/leonos-block/sda/sda1/size", value, sizeof(value));
    assert(!strcmp(value, "32768\n"));
    read_value("/sys/devices/platform/leonos-block/sda/uevent", value, sizeof(value));
    assert(!strcmp(value, "MAJOR=259\nMINOR=0\nDEVNAME=sda\nDEVTYPE=disk\n"));
    assert(proc_readlink("/sys/dev/block/259:1", value, sizeof(value)) > 0);
    read_value("/proc/sys/kernel/ostype", value, sizeof(value));
    assert(!strcmp(value, "ReliefNT\n"));
    /* Release interfaces must match the system-info record, including suffixes. */
    read_value("/proc/sys/kernel/osrelease", value, sizeof(value));
    assert(!strcmp(value, "9.8.7-0123\n"));
    read_value("/proc/sys/kernel/version", value, sizeof(value));
    assert(!strcmp(value, "2026-09-11 01:02:03\n"));
    read_value("/proc/version", value, sizeof(value));
    assert(!strcmp(value, "ReliefNT version 9.8.7-0123 (2026-09-11 01:02:03)\n"));
    read_value("/proc/cpuinfo", value, sizeof(value));
    assert(strstr(value, "processor\t: 0\n") && strstr(value, "model name\t:") &&
           strstr(value, "cpu cores\t: 1\n"));
    assert(proc_lookup("/proc/self/comm", &node) == 0);
    read_value("/proc/self/comm", value, sizeof(value));
    assert(!strcmp(value, "test\n"));
    read_value("/proc/self/stat", value, sizeof(value));
    assert(strstr(value, "42 (test) R 0 0 0 0 -1 "));
    char *fields = strrchr(value, ')') + 4;
    unsigned count = 0;
    for (char *part = strtok(fields, " \n"); part; part = strtok(NULL, " \n"))
        ++count;
    assert(count == 49); /* fields 4..52 */
    const char argv[] = "fastfetch\0--format\0json\0";
    memcpy(argument_pages + 4091, argv, sizeof(argv) - 1);
    current.as.cr3 = 4096;
    sched_task_mm(&current)->arg_start = 0x400ffb;
    sched_task_mm(&current)->arg_end = sched_task_mm(&current)->arg_start + sizeof(argv) - 1;
    uint32_t got;
    assert(proc_read("/proc/self/cmdline", 0, value, sizeof(value), &got) == 0 &&
           got == sizeof(argv) - 1 && !memcmp(value, argv, got));
    argument_pages[4091] = 'F';
    assert(proc_read("/proc/self/cmdline", 0, value, 3, &got) == 0 && got == 3 &&
           !memcmp(value, "Fas", 3));
    assert(proc_read("/proc/self/cmdline", 5, value, 9, &got) == 0 && got == 9 &&
           !memcmp(value, argv + 5, 9));
    uint8_t table[80] = {1, 27, 0, 0, 1, 2, 3, 4};
    for (unsigned i = 0; i < 16; ++i)
        table[8 + i] = i + 1;
    const char strings[] = "Vendor\0Product\0Version\0Serial\0\0";
    memcpy(table + 27, strings, sizeof(strings));
    assert(parse_smbios_table((uintptr_t)table, 27 + sizeof(strings), true) == 0);
    read_value("/sys/devices/virtual/dmi/id/product_name", value, sizeof(value));
    assert(!strcmp(value, "Product\n"));
    read_value("/sys/devices/virtual/dmi/id/product_uuid", value, sizeof(value));
    assert(!strcmp(value, "04030201-0605-0807-090a-0b0c0d0e0f10\n"));
    uint8_t broken[] = {1, 4, 0, 0, 'x'};
    assert(parse_smbios_table((uintptr_t)broken, sizeof(broken), true) < 0);
    assert(proc_lookup("/sys/devices/virtual/dmi/id/board_name", &node) == -2);
    read_value("/sys/devices/system/cpu/online", value, sizeof(value));
    assert(!strcmp(value, "0\n"));
    read_value("/sys/devices/pci0000:00/0000:00:02.0/modalias", value, sizeof(value));
    assert(!strcmp(value, "pci:v00001234d00001111sv00001af4sd00001100bc03sc00i00\n"));
    read_value("/sys/devices/pci0000:00/0000:00:02.0/uevent", value, sizeof(value));
    assert(strstr(value, "PCI_SUBSYS_ID=1AF4:1100\n"));
    assert(proc_lookup("/sys/class/drm/card0", &node) == -2);
    read_value("/sys/devices/virtual/graphics/fb0/virtual_size", value, sizeof(value));
    assert(!strcmp(value, "1280,720\n"));
    fixture_fb.width = 1920;
    fixture_fb.height = 1080;
    read_value("/sys/devices/virtual/graphics/fb0/virtual_size", value, sizeof(value));
    assert(!strcmp(value, "1920,1080\n"));
    /* A real DRM card is associated with its enumerated PCI function. The
     * PCI subsystem link is required by pinned libdrm's drmGetDevice2(). */
    fixture_drm_present = true;
    fixture_render_present = false;
    assert(proc_lookup("/sys/class/drm/card0", &node) == 0);
    assert(proc_lookup("/sys/class/drm/renderD128", &node) == -2);
    int subsystem_length = proc_readlink(
        "/sys/devices/pci0000:00/0000:00:02.0/subsystem", value, sizeof(value));
    assert(subsystem_length == (int)strlen("../../../bus/pci"));
    value[subsystem_length] = 0;
    assert(!strcmp(value, "../../../bus/pci"));
    assert(proc_lookup("/sys/bus/pci", &node) == 0 &&
           node.type == RELIEFOS_FS_TYPE_DIR);
    assert(proc_lookup("/sys/class/drm/card0-Virtual-1", &node) == 0 &&
           node.type == RELIEFOS_FS_TYPE_SYMLINK);
    assert(proc_lookup("/sys/devices/pci0000:00/0000:00:02.0/drm/card0/card0-Unknown-1",
                       &node) == -2);
    read_value("/sys/devices/pci0000:00/0000:00:02.0/drm/card0/card0-Virtual-1/status",
               value, sizeof(value));
    assert(!strcmp(value, "connected\n"));
    read_value("/sys/devices/pci0000:00/0000:00:02.0/drm/card0/card0-Virtual-1/modes",
               value, sizeof(value));
    assert(!strcmp(value, "1920x1080\n"));
    read_value("/sys/devices/pci0000:00/0000:00:02.0/drm/card0/card0-Virtual-1/enabled",
               value, sizeof(value));
    assert(!strcmp(value, "disabled\n"));
    fixture_kms_enabled = true;
    read_value("/sys/devices/pci0000:00/0000:00:02.0/drm/card0/card0-Virtual-1/enabled",
               value, sizeof(value));
    assert(!strcmp(value, "enabled\n"));
    fixture_kms_enabled = false;
    fixture_fb.width = 1920;
    fixture_fb.height = 1080;
    read_value("/sys/devices/pci0000:00/0000:00:02.0/drm/card0/card0-Virtual-1/modes",
               value, sizeof(value));
    assert(!strcmp(value, "1920x1080\n"));

    /* Render inventory exists only with the real 3D readiness predicate and
     * shares card0's PCI function without impersonating the primary node. */
    fixture_render_present = true;
    assert(proc_lookup("/sys/devices/pci0000:00/0000:00:02.0/drm/renderD128",
                       &node) == 0 && node.type == RELIEFOS_FS_TYPE_DIR);
    read_value("/sys/devices/pci0000:00/0000:00:02.0/drm/renderD128/dev",
               value, sizeof(value));
    assert(!strcmp(value, "226:128\n"));
    read_value("/sys/devices/pci0000:00/0000:00:02.0/drm/renderD128/uevent",
               value, sizeof(value));
    assert(!strcmp(value,
                   "MAJOR=226\nMINOR=128\nDEVNAME=dri/renderD128\n"
                   "DEVTYPE=drm_minor\n"));
    const char *render_target =
        "../../devices/pci0000:00/0000:00:02.0/drm/renderD128";
    assert(proc_readlink("/sys/class/drm/renderD128", value,
                         sizeof(value)) == (int)strlen(render_target));
    value[strlen(render_target)] = 0;
    assert(!strcmp(value, render_target));
    assert(proc_readlink("/sys/dev/char/226:128", value,
                         sizeof(value)) == (int)strlen(render_target));
    value[strlen(render_target)] = 0;
    assert(!strcmp(value, render_target));
    assert(proc_readlink(
               "/sys/devices/pci0000:00/0000:00:02.0/drm/renderD128/device",
               value, sizeof(value)) == (int)strlen("../.."));
    value[strlen("../..")]=0;
    assert(!strcmp(value, "../.."));
    fixture_render_present = false;
    assert(proc_lookup("/sys/class/drm/renderD128", &node) == -2);
    fixture_drm_present = false;
    assert(proc_readlink("/sys/class/dmi/id", value, 3) == 3 && !memcmp(value, "../", 3));
    walk("/sys", 0);
    fixture_cpus=4;
    inventory[2]=inventory[0]; inventory[2].core_id=inventory[0].core_id+1;
    read_value("/proc/cpuinfo",value,sizeof(value));
    assert(strstr(value,"processor\t: 2\n") && !strstr(value,"processor\t: 1\n") && strstr(value,"cpu cores\t: 2\n"));
    read_value("/sys/devices/system/cpu/online",value,sizeof(value)); assert(!strcmp(value,"0,2\n"));
    read_value("/sys/devices/system/cpu/offline",value,sizeof(value)); assert(!strcmp(value,"1,3\n"));
    read_value("/sys/devices/system/cpu/present",value,sizeof(value)); assert(!strcmp(value,"0,1,2,3\n"));
    struct task other=current; other.pid=77; other.tgid=77; other.uid=other.euid=other.suid=1001;
    assert(!proc_stat_mm_visible(&current,&other));
    current.cap_effective=1ULL<<CAP_SYS_PTRACE;
    assert(proc_stat_mm_visible(&current,&other));
    fixture_fb.available = false;
    assert(proc_lookup("/sys/class/graphics/fb0", &node) == -2);
    puts("PASS inventory: CPUID, SMBIOS bounds/UUID, PCI, live display modes, proc stat/comm, chunked "
         "reads and namespace walk");
}
