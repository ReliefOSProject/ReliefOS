/* Exercise the updater's real fstab parser without mounting host disks. */
#include "../../userland/apps/installer/installer_update_target.c"
#include <assert.h>

static const char *identity = "12345678-1234-4567-89ab-123456789abc";

static int check(const char *root, const char *text, int namespaced)
{
    char path[512], source[128] = {0};
    snprintf(path, sizeof(path), "%s/etc/fstab", root);
    FILE *file = fopen(path, "w");
    assert(file && fputs(text, file) >= 0 && !fclose(file));
    struct installer_update_target target = {0};
    assert(strlen(root) < sizeof(target.root));
    strcpy(target.root, root);
    strcpy(target.info.root_uuid, identity);
    int result = read_boot_layout(&target, source);
    if (!result) {
        assert(target.namespaced == namespaced);
        assert(!strcmp(source, "PARTUUID=esp"));
    }
    return result;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    char path[512];
    snprintf(path, sizeof(path), "%s/etc", argv[1]);
    assert(!mkdir(path, 0700));
    const char *physical = "PARTUUID=esp /efi vfat defaults 0 2\n";
    const char *legacy = "PARTUUID=esp /boot vfat defaults 0 2\n";
    char binding[256], combined[1024];
    snprintf(binding, sizeof(binding), "/efi/reliefos/instances/%s /boot none bind 0 0\n", identity);
    snprintf(combined, sizeof(combined), "%s%s", physical, binding);
    assert(!check(argv[1], combined, 1));
    assert(!check(argv[1], legacy, 0));
    assert(check(argv[1], physical, 0) == -EINVAL);
    snprintf(combined, sizeof(combined), "%s%s", legacy, binding);
    assert(check(argv[1], combined, 0) == -EINVAL);
    snprintf(combined, sizeof(combined), "%s%s%s", physical, physical, binding);
    assert(check(argv[1], combined, 0) == -ENOTUNIQ);
    snprintf(combined, sizeof(combined), "%s%s%s", physical, binding, binding);
    assert(check(argv[1], combined, 0) == -ENOTUNIQ);
    snprintf(combined, sizeof(combined), "%s/efi/reliefos/instances/00000000-0000-0000-0000-000000000001 /boot none bind 0 0\n", physical);
    assert(check(argv[1], combined, 0) == -EXDEV);
    snprintf(combined, sizeof(combined), "%s/efi/../outside /boot none bind 0 0\n", physical);
    assert(check(argv[1], combined, 0) == -EXDEV);
    snprintf(combined, sizeof(combined), "%s/foreign /boot ext4 defaults 0 2\n", legacy);
    assert(check(argv[1], combined, 0) == -EINVAL);
    snprintf(combined, sizeof(combined), "  # ignored comment /efi vfat\n%s", legacy);
    assert(!check(argv[1], combined, 0));
    puts("updater boot layout: shared ESP, legacy, duplicate, foreign and malformed bindings passed");
    return 0;
}
