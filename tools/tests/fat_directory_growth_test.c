#define main unused_ext_directory_fixture_main
#include "storage_mkdir_mount_test.c"
#undef main

/** @brief Grow real FAT directories with an LFN that cannot fit the old tail. */
int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *file = fopen(argv[1], "rb"); assert(file);
    assert(!fseek(file, 0, SEEK_END)); long size = ftell(file); assert(size > 0);
    rewind(file);
    g_active_volume = &g_volumes[0];
    *g_active_volume = (struct storage_volume){.volume_id = 0, .kind = STORAGE_VOLUME_RAM,
        .ram_bytes = size, .esp_sector_count = size / 512};
    g_storage.ram_base = malloc(size); assert(g_storage.ram_base);
    assert(fread(g_storage.ram_base, 1, size, file) == (size_t)size); fclose(file);
    strcpy(g_storage.mount_path, "/");
    assert(fat32_mount() == 0 && g_storage.cluster_bytes == 512);
    g_storage.ready = true; storage_cache_invalidate();
    assert(storage_mkdir("/mods") == 0);
    for (unsigned i = 0; i < 12; ++i) {
        char path[80]; snprintf(path, sizeof(path), "/mods/s%07u", i);
        assert(storage_write_file(path, "x", 1) == 0);
    }
    assert(storage_write_file("/mods/gfxterm_menu.mod", "module", 6) == 0);
    struct storage_node node;
    int ret = storage_lookup_path("/mods/gfxterm_menu.mod", &node);
    fprintf(stderr, "LFN after tail expansion lookup=%d\n", ret);
    assert(ret == 0 && node.size == 6);
    for (unsigned i = 0; i < 300; ++i) {
        char path[100]; snprintf(path, sizeof(path), "/mods/long-module-name-%04u.mod", i);
        assert(storage_write_file(path, "abc", 3) == 0);
        assert(storage_lookup_path(path, &node) == 0 && node.size == 3);
    }
    file = fopen(argv[1], "wb"); assert(file);
    assert(fwrite(g_storage.ram_base, 1, size, file) == (size_t)size); assert(!fclose(file));
    free(g_storage.ram_base);
    puts("PASS FAT directory growth: short/LFN entries remain visible across incomplete cluster tails");
    return 0;
}
