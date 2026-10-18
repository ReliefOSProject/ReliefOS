#!/bin/sh
set -eu

src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-$src/out/tmp}/reliefos-driverinfo-tests.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

if [ ! -f "$src/userland/apps/driverinfo/main.c" ]; then
    printf 'FAIL - driverinfo implementation is missing\n' >&2
    exit 1
fi

cat >"$work/stub.c" <<'EOF'
#include <reliefos/devmgr_service.h>
#include <string.h>

static const system_driver_info_t drivers[] = {
    {
        .id = 7,
        .state = SYSTEM_DRIVER_STATE_LOADED,
        .kind = RELIEFOS_DRIVER_KIND_AUDIO,
        .flags = RELIEFOS_DRIVER_FLAG_BUILTIN | RELIEFOS_DRIVER_FLAG_AUTOSTART,
        .abi_version = 1,
        .version = 3,
        .load_address = 0x12345000ULL,
        .image_size = 8192,
        .file = "hda.drv",
        .name = "Intel HD Audio",
    },
    {
        .id = 8,
        .state = SYSTEM_DRIVER_STATE_DISABLED,
        .kind = RELIEFOS_DRIVER_KIND_NETWORK,
        .flags = SYSTEM_DRIVER_FLAG_DISABLED,
        .abi_version = 1,
        .load_address = 0x20000000ULL,
        .image_size = 4096,
        .file = "legacy-net.drv",
        .name = "Legacy Ethernet",
    },
    {
        .id = 9,
        .state = SYSTEM_DRIVER_STATE_FAILED,
        .kind = 99,
        .abi_version = 2,
        .file = "vesa.drv",
        .name = "VESA display",
        .error = "probe failed",
    },
};

static const system_device_info_t devices[] = {
    {
        .device_class = SYSTEM_DEVICE_CLASS_AUDIO,
        .flags = SYSTEM_DEVICE_FLAG_PRESENT | SYSTEM_DEVICE_FLAG_ACTIVE,
        .name = "dsp",
        .status = "Running",
        .detail = "Intel HD Audio playback",
    },
    {
        .device_class = SYSTEM_DEVICE_CLASS_NETWORK,
        .flags = SYSTEM_DEVICE_FLAG_PRESENT | SYSTEM_DEVICE_FLAG_ACTIVE,
        .name = "ethernet0",
        .status = "Running",
        .detail = "Intel-compatible Ethernet",
    },
    {
        .device_class = SYSTEM_DEVICE_CLASS_DISPLAY,
        .flags = SYSTEM_DEVICE_FLAG_PRESENT,
        .name = "fb0",
        .status = "Stopped",
        .detail = "Inactive framebuffer",
    },
    {
        .device_class = 99,
        .flags = SYSTEM_DEVICE_FLAG_PRESENT | SYSTEM_DEVICE_FLAG_ACTIVE,
        .name = "sensor0",
        .status = "Running",
        .detail = "Auxiliary sensor",
    },
    {
        .device_class = 99,
        .flags = SYSTEM_DEVICE_FLAG_PRESENT | SYSTEM_DEVICE_FLAG_ACTIVE,
        .name = "sensor1",
        .status = "Running",
        .detail = "Secondary auxiliary sensor",
    },
};

int system_driver_list(system_driver_info_t *out, uint32_t capacity,
                       uint32_t *out_count)
{
    uint32_t count = (uint32_t)(sizeof(drivers) / sizeof(drivers[0]));
    if (out_count) *out_count = count;
    if (out && capacity) {
        uint32_t copy = count < capacity ? count : capacity;
        memcpy(out, drivers, copy * sizeof(*out));
    }
    return 0;
}

int system_device_list(system_device_info_t *out, uint32_t capacity,
                       uint32_t *out_count)
{
    uint32_t count = (uint32_t)(sizeof(devices) / sizeof(devices[0]));
    if (out_count) *out_count = count;
    if (out && capacity) {
        uint32_t copy = count < capacity ? count : capacity;
        memcpy(out, devices, copy * sizeof(*out));
    }
    return 0;
}
EOF

cat >"$work/fail.c" <<'EOF'
#include <reliefos/devmgr_service.h>

int system_driver_list(system_driver_info_t *out, uint32_t capacity,
                       uint32_t *out_count)
{
    (void)out;
    (void)capacity;
    if (out_count) *out_count = 0;
    return -1;
}

int system_device_list(system_device_info_t *out, uint32_t capacity,
                       uint32_t *out_count)
{
    (void)out;
    (void)capacity;
    if (out_count) *out_count = 0;
    return -1;
}
EOF

cc=${HOSTCC:-cc}
common_flags='-std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -Wformat=2 -Wshadow -Wstrict-prototypes'
"$cc" $common_flags \
    -I"$src/include" -I"$src/userland/runtime/include" \
    -I"$src/kernel/reliefnt/include/uapi" \
    "$src/userland/apps/driverinfo/main.c" "$work/stub.c" -o "$work/driverinfo"

"$work/driverinfo" >"$work/default.out"
grep -Fq 'ReliefOS active drivers' "$work/default.out"
grep -Fq 'hda.drv' "$work/default.out"
grep -Fq 'ID: 7' "$work/default.out"
grep -Fq 'Kind: Audio' "$work/default.out"
grep -Fq 'Load address: 0x12345000' "$work/default.out"
grep -Fq 'Image size: 8192 bytes' "$work/default.out"
grep -Fq 'Audio' "$work/default.out"
grep -Fq 'ethernet0' "$work/default.out"
grep -Fq 'sensor0' "$work/default.out"
grep -Fq 'sensor1' "$work/default.out"
test "$(grep -c '^  Other:$' "$work/default.out")" -eq 1
if grep -Fq 'legacy-net.drv' "$work/default.out" ||
   grep -Fq 'vesa.drv' "$work/default.out" ||
   grep -Fq 'fb0' "$work/default.out"; then
    printf 'default mode included inactive driver or device\n' >&2
    exit 1
fi

"$work/driverinfo" -a >"$work/all.out"
grep -Fq 'legacy-net.drv' "$work/all.out"
grep -Fq 'Disabled' "$work/all.out"
grep -Fq 'vesa.drv' "$work/all.out"
grep -Fq 'probe failed' "$work/all.out"
grep -Fq 'fb0' "$work/all.out"
grep -Fq 'ID: 8' "$work/all.out"
grep -Fq 'Kind: Network' "$work/all.out"
grep -Fq 'Load address: 0x20000000' "$work/all.out"
grep -Fq 'Image size: 4096 bytes' "$work/all.out"
grep -Fq 'Kind: Unknown (99)' "$work/all.out"

if "$work/driverinfo" --bad >"$work/usage.out" 2>&1; then
    printf 'invalid option unexpectedly succeeded\n' >&2
    exit 1
fi
grep -Fq 'Usage:' "$work/usage.out"

"$cc" $common_flags \
    -I"$src/include" -I"$src/userland/runtime/include" \
    -I"$src/kernel/reliefnt/include/uapi" \
    "$src/userland/apps/driverinfo/main.c" "$work/fail.c" -o "$work/driverinfo-fail"
if "$work/driverinfo-fail" >"$work/fail.out" 2>&1; then
    printf 'query failure unexpectedly succeeded\n' >&2
    exit 1
fi
grep -Fq 'failed' "$work/fail.out"

printf 'driverinfo filtering, device reporting, argument validation and failures passed\n'
