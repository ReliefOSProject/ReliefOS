#define _GNU_SOURCE 1
#include "../../userland/runtime/src/bootloader_internal.h"
#include <reliefos/blockdev.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
int reliefos_block_get_info(const char *p, struct reliefos_block_disk_info *out) {
    (void)p; (void)out; assert(0); return -ENOSYS;
}
int reliefos_block_list_partitions(const char *p, struct reliefos_block_partition *out, uint32_t n, uint32_t *count) {
    (void)p; (void)out; (void)n; (void)count; assert(0); return -ENOSYS;
}
int reliefos_block_partition_uuid(const char *p, uint32_t n, char out[37]) {
    (void)p; (void)n; (void)out; assert(0); return -ENOSYS;
}
static void directory(const char *base, const char *relative) {
    char path[1024]; snprintf(path, sizeof(path), "%s/%s", base, relative);
    assert(!mkdir(path, 0755) || errno == EEXIST);
}
static void write_file(const char *base, const char *relative, const char *text) {
    char path[1024]; snprintf(path, sizeof(path), "%s/%s", base, relative);
    FILE *file = fopen(path, "w"); assert(file);
    assert(fputs(text, file) >= 0); assert(!fclose(file));
}
static void equal(const char *base, const char *relative, const char *text) {
    char path[1024], bytes[2048]; snprintf(path, sizeof(path), "%s/%s", base, relative);
    FILE *file = fopen(path, "r"); assert(file);
    size_t n = fread(bytes, 1, sizeof(bytes) - 1, file); bytes[n] = 0;
    assert(!ferror(file) && !fclose(file)); assert(!strcmp(text, bytes));
}
int main(int argc, char **argv) {
    assert(argc == 2);
    const char *uuids[] = {"11111111-1111-1111-1111-111111111111", "22222222-2222-2222-2222-222222222222"};
    char source[1024], esp[1024], instance[1024];
    snprintf(source, sizeof(source), "%s/source", argv[1]); assert(!mkdir(source, 0755));
    directory(source, "EFI"); directory(source, "EFI/BOOT");
    directory(source, "reliefos"); directory(source, "leonos"); directory(source, "grub");
    write_file(source, "EFI/BOOT/BOOTX64.EFI", "verified loader one");
    snprintf(esp, sizeof(esp), "%s/shared", argv[1]); assert(!mkdir(esp, 0755));
    directory(esp, "EFI"); directory(esp, "EFI/BOOT"); directory(esp, "EFI/Microsoft");
    write_file(esp, "EFI/BOOT/BOOTX64.EFI", "foreign fallback");
    write_file(esp, "EFI/BOOT/grub.cfg", "foreign config");
    write_file(esp, "EFI/Microsoft/bootmgfw.efi", "Windows loader");
    struct reliefos_boot_install_options options = {.source=source, .install_uefi=1};
    for (unsigned i=0; i<2; ++i) {
        assert(!reliefos_boot_prepare_instance(esp, uuids[i], instance, sizeof(instance)));
        directory(instance, "grub");
        options.boot_dir=instance; options.root_partuuid=uuids[i];
        directory(instance, "EFI"); directory(instance, "EFI/BOOT");
        write_file(instance, "EFI/BOOT/BOOTX64.EFI", "interrupted private loader publication");
        assert(!reliefos_boot_validate_target(&options));
        assert(!boot_validate_instance(&options, esp));
        assert(!boot_install_uefi(&options, esp));
        equal(esp, "EFI/BOOT/BOOTX64.EFI", "foreign fallback");
        equal(esp, "EFI/BOOT/grub.cfg", "foreign config");
        equal(esp, "EFI/Microsoft/bootmgfw.efi", "Windows loader");
        char loader[256]; snprintf(loader, sizeof(loader), "EFI/ReliefOS/%s/grubx64.efi", uuids[i]);
        equal(esp, loader, "verified loader one");
    }
    assert(reliefos_boot_prepare_instance(esp, "../../escape", instance, sizeof(instance)) == -EINVAL);
    snprintf(esp, sizeof(esp), "%s/empty", argv[1]); assert(!mkdir(esp, 0755));
    assert(!reliefos_boot_prepare_instance(esp, uuids[0], instance, sizeof(instance)));
    directory(instance, "grub"); options.boot_dir=instance; options.root_partuuid=uuids[0];
    assert(!boot_install_uefi(&options, esp));
    equal(esp, "EFI/BOOT/BOOTX64.EFI", "verified loader one");
    write_file(source, "EFI/BOOT/BOOTX64.EFI", "verified loader two");
    assert(!boot_install_uefi(&options, esp));
    equal(esp, "EFI/BOOT/BOOTX64.EFI", "verified loader two");
    write_file(esp, "EFI/BOOT/BOOTX64.EFI", "new foreign fallback");
    assert(!boot_install_uefi(&options, esp));
    equal(esp, "EFI/BOOT/BOOTX64.EFI", "new foreign fallback");
    char bad[1024]; snprintf(bad, sizeof(bad), "%s/foreign", argv[1]); assert(!mkdir(bad, 0755));
    assert(boot_validate_instance(&options, bad) < 0);
    write_file(instance, "reliefos-instance.id", "not owned by ReliefOS");
    assert(reliefos_boot_prepare_instance(esp, uuids[0], instance, sizeof(instance)) < 0);
    puts("UEFI: shared ESP, two instances, fallback ownership, updates and namespace refusal passed");
}
