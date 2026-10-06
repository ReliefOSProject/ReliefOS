/* Exercise the real storage facade and ext-family backend, without a VM. */
#define main legacy_mkdir_fixture_main
#include "storage_mkdir_mount_test.c"
#undef main

/** @brief Release the RAM image and the backend journal after each run. */
static void finish_volume(void)
{
    storage_ext4_journal_close(&g_volumes[0]);
    free(g_volumes[0].ram_base);
}

int main(int argc, char **argv)
{
    assert(argc == 2 || argc == 3);
    load_volume(argv[1], 0, "/");
    g_active_volume = &g_volumes[0];

    /* Bypass the facade once to prove the disk backend accepts this name. */
    const char *backend_path = "/A:0-backend";
    int backend_ret = storage_ext4_replace(&g_storage, backend_path, "cookie", 6);
    printf("native ext-family create %s: %d\n", backend_path, backend_ret);
    fflush(stdout);
    assert(backend_ret == 0);
    struct storage_node node;
    assert(storage_ext4_ops.lookup(&g_storage, backend_path, &node) == 0);
    char content[8];
    uint32_t got;
    assert(storage_read_node(&node, 0, content, sizeof(content), &got) == 0);
    assert(got == 6 && !memcmp(content, "cookie", 6));
    puts("PASS native ext-family backend creates and reads A:0-backend");
    fflush(stdout);
    if (argc == 3 && !strcmp(argv[2], "--backend-only")) {
        finish_volume();
        return 0;
    }

    char resolved[RELIEFOS_FS_PATH_LEN];
    int ret = storage_resolve_path("/", "/var/lib/xdm/authdir/authfiles/A:0-XXXXXX",
                                   resolved, sizeof(resolved));
    printf("XDM authentication pathname resolution: %d (expected 0)\n", ret);
    fflush(stdout);
    assert(ret == 0);
    assert(!strcmp(resolved, "/var/lib/xdm/authdir/authfiles/A:0-XXXXXX"));
    assert(storage_resolve_path("/session:0", "./A:0-cookie", resolved, sizeof(resolved)) == 0);
    assert(!strcmp(resolved, "/session:0/A:0-cookie"));
    assert(storage_resolve_path("/", "/A\\0", resolved, sizeof(resolved)) == -22);
    assert(storage_resolve_path("/A\\0", "file", resolved, sizeof(resolved)) == -22);

    char parent[RELIEFOS_FS_PATH_LEN], name[RELIEFOS_FS_NAME_LEN];
    assert(sidecar_parent_name("/session:0/A:0-cookie", parent, sizeof(parent),
                                name, sizeof(name)) == 0);
    assert(!strcmp(parent, "/session:0") && !strcmp(name, "A:0-cookie"));
    assert(sidecar_parent_name("relative:A", parent, sizeof(parent), name, sizeof(name)) == -22);
    assert(fat32_validate_name("A:0-cookie") == -22);
    assert(fat32_validate_name("A-0-cookie") == 0);
    uint16_t utf16[255];
    uint32_t utf16_len;
    assert(exfat_utf8_name("A:0-cookie", utf16, &utf16_len) == -22);
    assert(exfat_utf8_name("A-0-cookie", utf16, &utf16_len) == 0);

    assert(storage_mkdir("/session:0") == 0);
    assert(storage_write_file("/session:0/A:0-cookie", "secret", 6) == 0);
    assert(storage_lookup_path("/session:0/A:0-cookie", &node) == 0);
    struct reliefos_permissions permissions = {.mode = 0600, .uid = 1000, .gid = 1000};
    assert(storage_inode_permissions(&node, &permissions, true) == 0);
    memset(&permissions, 0, sizeof(permissions));
    assert(storage_inode_permissions(&node, &permissions, false) == 0);
    assert(permissions.mode == 0600 && permissions.uid == 1000 && permissions.gid == 1000);
    assert(storage_read_node(&node, 0, content, sizeof(content), &got) == 0);
    assert(got == 6 && !memcmp(content, "secret", 6));
    assert(storage_rename("/session:0/A:0-cookie", "/session:0/B:1-cookie") == 0);
    assert(storage_lookup_path("/session:0/A:0-cookie", &node) == -2);
    assert(storage_lookup_path("/session:0/B:1-cookie", &node) == 0);
    assert(storage_unlink("/session:0/B:1-cookie") == 0);
    assert(storage_lookup_path("/session:0/B:1-cookie", &node) == -2);
    assert(storage_sync_all() == 0);
    finish_volume();
    puts("PASS colon paths: resolve, create, read, permissions, rename, unlink; FAT/exFAT and backslash limits preserved");
    return 0;
}
