#include "volumes.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <initializer_list>

extern "C" int reliefos_block_probe_filesystem(const char *, uint32_t *) { return -ENOSYS; }

int main()
{
    using namespace fm;
    uint64_t value = 7;
    assert(number("18446744073709551615", value) && value == UINT64_MAX);
    assert(!number("18446744073709551616", value) && value == UINT64_MAX);
    for (const char *text : {"", "-1", "+1", " 1", "1\n", "1x"}) assert(!number(text, value));
    for (const char *text : {"sdb", "sdb1", "nvme1n1p2", "mmcblk0p1", "dm-1"}) assert(device_name(text));
    for (const char *text : {"", ".", "..", "../sda", "/dev/sda", "sda\n", "sda;id", "sda x"})
        assert(!device_name(text));
    assert(path_under("/media/disk/dir", "/media/disk"));
    assert(path_under("/media/disk", "/media/disk"));
    assert(!path_under("/media/disk2", "/media/disk"));
    assert(!path_under("/media/disk", ""));
    assert(path_under("/media", "/"));
    Text mountpoint, filesystem;
    uint64_t device;
    uint64_t mount_id, parent_id;
    assert(parse_mountinfo("12 1 8:3 / /media/u\\040disk rw,nosuid shared:1 - vfat /dev/sdb1 rw\n",
                           device, mountpoint, filesystem, &mount_id, &parent_id));
    assert(device == 2051 && mountpoint == "/media/u disk" && filesystem == "vfat");
    assert(mount_id == 12 && parent_id == 1);
    assert(mount_before(2, 4, 1, 12));
    assert(mount_before(1, 12, 1, 6));
    assert(!mount_before(1, 6, 1, 12));
    assert(!mount_before(1, 12, 2, 4));
    assert(!parse_mountinfo("12 1 8:3 / relative rw - vfat /dev/sdb1 rw\n", device, mountpoint, filesystem));
    assert(!parse_mountinfo("12 1 8:3 / /media/\\000disk rw - vfat /dev/sdb1 rw\n", device, mountpoint, filesystem));

    const char *valid = "sdb1\tsdb\t42\t2051\t67108864\t1\t0\t0\t2f72756e2f6d656469612f75206469736b\t76666174\n";
    Array<Volume> volumes;
    assert(parse_volumes(valid, strlen(valid), volumes) && volumes.size() == 1);
    assert(volumes[0].mount == "/run/media/u disk" && volumes[0].filesystem == "vfat");
    assert(volumes[0].sequence == 42 && volumes[0].mounts == 1);
    Volume before = volumes[0];
    assert(same_volume(before, volumes[0]));
    ++before.sequence;
    assert(!same_volume(before, volumes[0]));
    assert(!parse_volumes(valid, strlen(valid) - 1, volumes) && volumes.size() == 1);
    const char *invalid[] = {
        "sdb1\tsdb\t0\t2051\t64\t0\t0\t0\t\t\n",
        "sdb1\tsdb\t42\t2051\t64\t1\t0\t0\t\t\n",
        "sdb1\tsdb\t42\t2051\t64\t0\t0\t0\t2f00\t\n",
        "sdb1\tsdb\t42\t2051\t64\t0\t2\t0\t\t\n",
        "sdb1\tsdb\t42\t2051\t64\t0\t0\t0\t\t\textra\n",
        "sdb1\tsdb\t42\t2051\t64\t1\t0\t0\t72656c\t\n"
    };
    for (const char *text : invalid) assert(!parse_volumes(text, strlen(text), volumes));
    char repeated[1024];
    snprintf(repeated, sizeof(repeated), "%s%s", valid, valid);
    assert(!parse_volumes(repeated, strlen(repeated), volumes));
    Text path, users;
    const char *busy = "BUSY\t123\t62617368\nERROR\t16\n";
    assert(parse_volume_reply(busy, strlen(busy), path, users) == -EBUSY);
    assert(strstr(users.c_str(), "PID 123  bash"));
    const char *ok = "OK\t2f72756e2f6d65646961\n";
    assert(!parse_volume_reply(ok, strlen(ok), path, users) && path == "/run/media");
    assert(parse_volume_reply(ok, strlen(ok) - 1, path, users) == -EPROTO);
    assert(!parse_volume_reply("OK\t\r\n", 5, path, users) && path.empty());
    assert(parse_volume_reply("ERROR\t0\n", 8, path, users) == -EPROTO);
    assert(parse_volumes("", 0, volumes) && volumes.empty());
    puts("PASS volume identity, numeric/path boundaries, escaped protocol, stale/truncated replies and busy diagnostics");
}
