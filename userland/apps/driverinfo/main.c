#include <reliefos/devmgr_service.h>
#include <reliefos/driver.h>
#include <reliefos/stdio.h>
#include <stdint.h>
#include <string.h>

#define T(s) (s)

static const uint32_t device_classes[] = {
    SYSTEM_DEVICE_CLASS_AUDIO,
    SYSTEM_DEVICE_CLASS_NETWORK,
    SYSTEM_DEVICE_CLASS_DISPLAY,
    SYSTEM_DEVICE_CLASS_STORAGE,
    SYSTEM_DEVICE_CLASS_INPUT,
    SYSTEM_DEVICE_CLASS_SERIAL,
    SYSTEM_DEVICE_CLASS_SYSTEM,
};

static void copy_field(char *dst, uint32_t capacity, const char *src,
                       uint32_t source_capacity)
{
    uint32_t index = 0;
    if (!dst || capacity == 0) {
        return;
    }
    while (src && index + 1U < capacity && index < source_capacity && src[index]) {
        dst[index] = src[index];
        ++index;
    }
    dst[index] = 0;
}

static const char *driver_state_name(uint32_t state)
{
    switch (state) {
    case SYSTEM_DRIVER_STATE_LOADING:
        return T("Loading");
    case SYSTEM_DRIVER_STATE_LOADED:
        return T("Loaded");
    case SYSTEM_DRIVER_STATE_DISABLED:
        return T("Disabled");
    case SYSTEM_DRIVER_STATE_FAILED:
        return T("Failed");
    default:
        return T("Unloaded");
    }
}

static const char *driver_kind_name(uint32_t kind)
{
    switch (kind) {
    case RELIEFOS_DRIVER_KIND_INPUT:
        return T("Input");
    case RELIEFOS_DRIVER_KIND_SERIAL:
        return T("Serial");
    case RELIEFOS_DRIVER_KIND_NETWORK:
        return T("Network");
    case RELIEFOS_DRIVER_KIND_AUDIO:
        return T("Audio");
    default:
        return 0;
    }
}

static const char *device_class_name(uint32_t device_class)
{
    switch (device_class) {
    case SYSTEM_DEVICE_CLASS_AUDIO:
        return T("Audio");
    case SYSTEM_DEVICE_CLASS_NETWORK:
        return T("Network");
    case SYSTEM_DEVICE_CLASS_DISPLAY:
        return T("Display");
    case SYSTEM_DEVICE_CLASS_STORAGE:
        return T("Storage");
    case SYSTEM_DEVICE_CLASS_INPUT:
        return T("Input");
    case SYSTEM_DEVICE_CLASS_SERIAL:
        return T("Serial");
    case SYSTEM_DEVICE_CLASS_SYSTEM:
        return T("System");
    default:
        return T("Other");
    }
}

static int driver_is_visible(const system_driver_info_t *driver, int show_all)
{
    return show_all || driver->state == SYSTEM_DRIVER_STATE_LOADED;
}

static int device_is_visible(const system_device_info_t *device, int show_all)
{
    return show_all || (device->flags & SYSTEM_DEVICE_FLAG_ACTIVE) != 0U;
}

static void print_driver_flags(uint32_t flags)
{
    int first = 1;
    printf("    Flags:");
    if (flags & RELIEFOS_DRIVER_FLAG_BUILTIN) {
        printf(" built-in");
        first = 0;
    }
    if (flags & RELIEFOS_DRIVER_FLAG_AUTOSTART) {
        printf("%sautostart", first ? " " : ", ");
        first = 0;
    }
    if (flags & RELIEFOS_DRIVER_FLAG_DISABLED) {
        printf("%sdisabled", first ? " " : ", ");
        first = 0;
    }
    if (first) {
        printf(" none");
    }
    printf("\n");
}

static void print_driver(const system_driver_info_t *driver)
{
    char file[RELIEFOS_DRIVER_FILE_LEN + 1U];
    char name[RELIEFOS_DRIVER_NAME_LEN + 1U];
    char error[RELIEFOS_DRIVER_ERROR_LEN + 1U];

    copy_field(file, sizeof(file), driver->file, sizeof(driver->file));
    copy_field(name, sizeof(name), driver->name, sizeof(driver->name));
    copy_field(error, sizeof(error), driver->error, sizeof(driver->error));
    if (!file[0]) {
        copy_field(file, sizeof(file), "(unnamed)", 9U);
    }
    if (!name[0]) {
        copy_field(name, sizeof(name), "(no description)", 16U);
    }

    printf("  %s - %s\n", file, name);
    printf("    ID: %u\n", driver->id);
    printf("    State: %s\n", driver_state_name(driver->state));
    {
        const char *kind = driver_kind_name(driver->kind);
        if (kind) {
            printf("    Kind: %s\n", kind);
        } else {
            printf("    Kind: Unknown (%u)\n", driver->kind);
        }
    }
    printf("    ABI: %u  Version: %u\n", driver->abi_version, driver->version);
    printf("    Load address: 0x%llx\n",
           (unsigned long long)driver->load_address);
    printf("    Image size: %llu bytes\n",
           (unsigned long long)driver->image_size);
    print_driver_flags(driver->flags);
    if (error[0]) {
        printf("    Error: %s\n", error);
    }
}

static uint32_t visible_driver_count(const system_driver_info_t *drivers,
                                     uint32_t count, int show_all)
{
    uint32_t visible = 0;
    for (uint32_t index = 0; index < count; ++index) {
        if (driver_is_visible(&drivers[index], show_all)) {
            ++visible;
        }
    }
    return visible;
}

static uint32_t visible_device_count(const system_device_info_t *devices,
                                     uint32_t count, int show_all)
{
    uint32_t visible = 0;
    for (uint32_t index = 0; index < count; ++index) {
        if (device_is_visible(&devices[index], show_all)) {
            ++visible;
        }
    }
    return visible;
}

static void print_devices_for_class(const system_device_info_t *devices,
                                    uint32_t count, uint32_t device_class,
                                    int show_all, uint32_t *printed)
{
    int printed_class = 0;
    for (uint32_t index = 0; index < count; ++index) {
        char name[RELIEFOS_DEVICE_NAME_LEN + 1U];
        char status[RELIEFOS_DEVICE_STATUS_LEN + 1U];
        char detail[RELIEFOS_DEVICE_DETAIL_LEN + 1U];
        const system_device_info_t *device = &devices[index];
        if (device->device_class != device_class ||
            !device_is_visible(device, show_all)) {
            continue;
        }
        if (!printed_class) {
            printf("  %s:\n", device_class_name(device_class));
            printed_class = 1;
        }
        copy_field(name, sizeof(name), device->name, sizeof(device->name));
        copy_field(status, sizeof(status), device->status, sizeof(device->status));
        copy_field(detail, sizeof(detail), device->detail, sizeof(device->detail));
        if (!name[0]) {
            copy_field(name, sizeof(name), "(unnamed)", 9U);
        }
        if (!status[0]) {
            copy_field(status, sizeof(status), "Unknown", 7U);
        }
        printf("    %s - %s\n", name, status);
        if (detail[0]) {
            printf("      %s\n", detail);
        }
        ++(*printed);
    }
}

static void print_devices(const system_device_info_t *devices, uint32_t count,
                          int show_all, uint32_t visible_count)
{
    uint32_t printed = 0;
    printf("Devices (%u):\n", visible_count);
    if (!visible_count) {
        printf("  (none)\n");
        return;
    }
    for (uint32_t index = 0;
         index < sizeof(device_classes) / sizeof(device_classes[0]); ++index) {
        print_devices_for_class(devices, count, device_classes[index], show_all,
                                &printed);
    }
    for (uint32_t index = 0; index < count; ++index) {
        int seen_class = 0;
        if (devices[index].device_class == SYSTEM_DEVICE_CLASS_AUDIO ||
            devices[index].device_class == SYSTEM_DEVICE_CLASS_NETWORK ||
            devices[index].device_class == SYSTEM_DEVICE_CLASS_DISPLAY ||
            devices[index].device_class == SYSTEM_DEVICE_CLASS_STORAGE ||
            devices[index].device_class == SYSTEM_DEVICE_CLASS_INPUT ||
            devices[index].device_class == SYSTEM_DEVICE_CLASS_SERIAL ||
            devices[index].device_class == SYSTEM_DEVICE_CLASS_SYSTEM ||
            !device_is_visible(&devices[index], show_all)) {
            continue;
        }
        for (uint32_t prior = 0; prior < index; ++prior) {
            if (devices[prior].device_class == devices[index].device_class &&
                device_is_visible(&devices[prior], show_all)) {
                seen_class = 1;
                break;
            }
        }
        if (seen_class) {
            continue;
        }
        print_devices_for_class(devices, count, devices[index].device_class,
                                show_all, &printed);
    }
}

static int parse_options(int argc, char **argv, int *show_all)
{
    if (!show_all) {
        return -1;
    }
    *show_all = 0;
    for (int index = 1; index < argc; ++index) {
        if (!strcmp(argv[index], "-a") || !strcmp(argv[index], "--all")) {
            *show_all = 1;
        } else {
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    system_driver_info_t drivers[SYSTEM_DRIVER_MAX];
    system_device_info_t devices[SYSTEM_DEVICE_MAX];
    uint32_t driver_count = 0;
    uint32_t device_count = 0;
    uint32_t shown_drivers;
    uint32_t shown_devices;
    int show_all = 0;
    int ret;

    if (parse_options(argc, argv, &show_all) < 0) {
        printf("Usage: driverinfo [-a|--all]\n");
        printf("  -a, --all  show disabled, failed and inactive entries too\n");
        return 2;
    }

    memset(drivers, 0, sizeof(drivers));
    memset(devices, 0, sizeof(devices));
    ret = system_driver_list(drivers, SYSTEM_DRIVER_MAX, &driver_count);
    if (ret < 0) {
        printf("driverinfo: failed to query drivers (return %d)\n", ret);
        return 1;
    }
    ret = system_device_list(devices, SYSTEM_DEVICE_MAX, &device_count);
    if (ret < 0) {
        printf("driverinfo: failed to query devices (return %d)\n", ret);
        return 1;
    }
    if (driver_count > SYSTEM_DRIVER_MAX) {
        driver_count = SYSTEM_DRIVER_MAX;
    }
    if (device_count > SYSTEM_DEVICE_MAX) {
        device_count = SYSTEM_DEVICE_MAX;
    }

    shown_drivers = visible_driver_count(drivers, driver_count, show_all);
    shown_devices = visible_device_count(devices, device_count, show_all);
    printf("ReliefOS active drivers\n");
    printf("=======================\n\n");
    printf("Mode: %s\n\n", show_all ? "all driver and device states"
                                      : "active drivers and devices");
    printf("Drivers (%u):\n", shown_drivers);
    if (!shown_drivers) {
        printf("  (none)\n");
    } else {
        for (uint32_t index = 0; index < driver_count; ++index) {
            if (driver_is_visible(&drivers[index], show_all)) {
                print_driver(&drivers[index]);
            }
        }
    }
    printf("\n");
    print_devices(devices, device_count, show_all, shown_devices);
    printf("\nSummary: %u %s, %u %s\n", shown_drivers,
           shown_drivers == 1U ? "driver" : "drivers", shown_devices,
           shown_devices == 1U ? "device" : "devices");
    return 0;
}
