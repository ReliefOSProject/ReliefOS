#include <assert.h>
#include <stdarg.h>
#include "../../userland/runtime/src/blockdev.c"
_Static_assert(BLKROGET == 0x125e, "Linux BLKROGET uses the historical _IO encoding");

static int fail_open, fail_ioctl, fail_seek, fail_io, virtio, partition;
static off_t read_offset;
static uint8_t gpt4k[6 * 4096], fs4k[4096];
int open(const char *path, int flags, ...)
{
    (void)flags;
    partition = virtio && !strcmp(path, "/dev/vda1");
    if (fail_open || (!partition && strcmp(path, virtio ? "/dev/vda" : "/dev/sda"))) {
        errno = fail_open ? EACCES : ENOENT;
        return -1;
    }
    return 42;
}
int close(int fd) { assert(fd == 42); errno = EBUSY; return 0; }
int ioctl(int fd, unsigned long request, ...)
{
    assert(fd == 42);
    if (fail_ioctl) { errno = EIO; return -1; }
    va_list args;
    va_start(args, request);
    if (request == BLKGETSIZE64)
        *va_arg(args, uint64_t *) = partition ? 12000ULL * 4096 : 2ULL * 1024 * 1024 * 1024;
    else if (request == BLKSSZGET) *va_arg(args, int *) = virtio ? 4096 : 512;
    else if (request == BLKROGET) *va_arg(args, int *) = 0;
    else assert(request == BLKRRPART);
    va_end(args);
    return 0;
}
off_t lseek(int fd, off_t offset, int whence)
{
    assert(fd == 42 && whence == SEEK_SET);
    if (fail_seek) { errno = ESPIPE; return -1; }
    read_offset = offset;
    return offset;
}
ssize_t read(int fd, void *buffer, size_t length)
{
    assert(fd == 42);
    if (fail_io) { errno = EIO; return -1; }
    memset(buffer, 0, length);
    if (virtio) {
        assert(read_offset >= 0 && read_offset % 4096 == 0 && length % 4096 == 0);
        const uint8_t *fixture = partition ? fs4k : gpt4k;
        size_t size = partition ? sizeof(fs4k) : sizeof(gpt4k);
        if ((uint64_t)read_offset < size) {
            assert(length <= size - (size_t)read_offset);
            memcpy(buffer, fixture + read_offset, length);
        }
    }
    read_offset += (off_t)length;
    return length;
}
ssize_t write(int fd, const void *buffer, size_t length)
{
    assert(fd == 42); (void)buffer;
    if (fail_io) { errno = ENOSPC; return -1; }
    return length;
}

int main(void)
{
    struct reliefos_block_disk_info disks[RELIEFOS_BLOCK_MAX_DISKS];
    uint32_t count = 0;
    assert(reliefos_block_list_disks(disks, RELIEFOS_BLOCK_MAX_DISKS, &count) == 0);
    assert(count == 1 && disks[0].sector_count == 4194304 && disks[0].sector_size == 512);
    fail_open = 1;
    assert(reliefos_block_get_info("/dev/sda", disks) == -EACCES);
    fail_open = 0; fail_ioctl = 1;
    assert(reliefos_block_get_info("/dev/sda", disks) == -EIO);
    assert(block_reread(42) == -EIO);
    fail_ioctl = 0;
    char buffer[512];
    fail_seek = 1;
    assert(block_io(42, 0, buffer, sizeof(buffer), 0) == -ESPIPE);
    fail_seek = 0; fail_io = 1;
    assert(block_io(42, 0, buffer, sizeof(buffer), 0) == -EIO);
    assert(block_io(42, 0, buffer, sizeof(buffer), 1) == -ENOSPC);
    fail_io = 0;
    virtio = 1;
    assert(!reliefos_block_list_disks(disks, RELIEFOS_BLOCK_MAX_DISKS, &count));
    assert(count == 1 && !strcmp(disks[0].path, "/dev/vda"));
    assert(disks[0].sector_count == 524288 && disks[0].sector_size == 4096);
    uint32_t index;
    assert(!block_disk_index("/dev/vdaa", &index) && index == 26);
    assert(block_disk_index("/dev/vda1", &index) == -EINVAL);
    assert(reliefos_block_gpt_initialize("/dev/vda", 1) == -ENOTSUP);
    {
        const uint64_t sectors = disks[0].sector_count;
        struct block_gpt_entry entry = {.first_lba = 256, .last_lba = 12255};
        memcpy(entry.type_guid, block_guid_linux, sizeof(entry.type_guid));
        entry.unique_guid[0] = 1;
        entry.name[0] = 'R';
        memcpy(gpt4k + 2 * 4096, &entry, sizeof(entry));
        struct block_gpt_header header = {
            .signature = UINT64_C(0x5452415020494645), .revision = 0x10000,
            .header_size = sizeof(header), .current_lba = 1, .backup_lba = sectors - 1,
            .first_usable_lba = 6, .last_usable_lba = sectors - 6, .disk_guid = {2},
            .partition_entries_lba = 2, .partition_entry_count = 128, .partition_entry_size = 128,
            .partition_entries_crc32 = block_crc32(gpt4k + 2 * 4096, 4 * 4096),
        };
        header.header_crc32 = block_crc32(&header, sizeof(header));
        memcpy(gpt4k + 4096, &header, sizeof(header));
        fs4k[1024 + 56] = 0x53; fs4k[1024 + 57] = 0xef;
        fs4k[1024 + 96] = 0x40; /* extents */
        struct reliefos_block_partition part;
        assert(!reliefos_block_list_partitions("/dev/vda", &part, 1, &count) && count == 1);
        assert(!strcmp(part.path, "/dev/vda1") && !strcmp(part.name, "R"));
        assert(part.first_lba == 256 && part.sector_count == 12000);
        assert(part.gpt_type == RELIEFOS_BLOCK_GPT_LINUX && part.filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXT4);
        char uuid[37];
        assert(!reliefos_block_partition_uuid("/dev/vda", 0, uuid));
        assert(!strcmp(uuid, "00000001-0000-0000-0000-000000000000"));
    }
    puts("PASS block device errors: missing disks, open/ioctl/seek/read/write and cleanup");
    return 0;
}
