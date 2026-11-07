#include "model.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

int main()
{
    using namespace dm;
    uint32_t size = 0;
    assert(parse_size("512", size) && size == 512);
    assert(parse_size("4294967295", size) && size == UINT32_MAX);
    const char *bad[] = {"", "0", "-1", "+1", "1.5", " 512", "512 ",
                         "4294967296", "99999999999999999999999999999", nullptr};
    for (const char **p = bad; *p; ++p) assert(!parse_size(*p, size));
    assert(confirmation_matches(Action::Create, "CREATE"));
    assert(confirmation_matches(Action::Format, "FORMAT"));
    assert(confirmation_matches(Action::Delete, "DELETE"));
    assert(!confirmation_matches(Action::Delete, "DELETE "));
    assert(!confirmation_matches(Action::Delete, "delete"));
    assert(!confirmation_matches(Action::Refresh, ""));

    Snapshot state{};
    state.disk_count = 1;
    state.selected_disk = 0;
    state.partition_count = 1;
    state.disks[0].info.sector_size = 512;
    state.disks[0].info.sector_count = 4194304;
    strcpy(state.disks[0].info.path, "/dev/sdb");
    state.disks[0].identity_valid = true;
    state.disks[0].header[56] = 17; // GPT disk GUID, independently of capacity.
    Disk target = state.disks[0];
    assert(same_disk(target, state.disks[0]));
    target.header[56] ^= 1;
    assert(!same_disk(target, state.disks[0]));
    target = state.disks[0];
    target.sequence = 123;
    assert(!same_disk(target, state.disks[0]));
    target = state.disks[0];
    target.identity_valid = false;
    assert(!same_disk(target, state.disks[0]));
    auto &part = state.partitions[0];
    strcpy(part.info.path, "/dev/sdb1");
    strcpy(part.uuid, "1176bcee-b108-469b-a5de-c68317a9214c");
    part.info.first_lba = 2048;
    part.info.sector_count = 524288;
    part.info.gpt_type = RELIEFOS_BLOCK_GPT_LINUX;
    part.info.filesystem = RELIEFOS_BLOCK_FILESYSTEM_EXT4;
    assert(action_allowed(state, Action::Create, -1, 0) == 0);
    assert(action_allowed(state, Action::Format, 0, 0) == 0);
    assert(action_allowed(state, Action::Delete, 0, 0) == 0);
    assert(action_allowed(state, Action::Mount, 0, 0) == 0);
    assert(action_allowed(state, Action::Unmount, 0, 0) == -EINVAL);
    assert(action_allowed(state, Action::Format, 0, 1000) == -EACCES);
    assert(action_allowed(state, Action::Format, -1, 0) == -EINVAL);
    assert(action_allowed(state, Action::Delete, 1, 0) == -EINVAL);

    state.disks[0].read_only = true;
    assert(action_allowed(state, Action::Create, -1, 0) == -EROFS);
    assert(action_allowed(state, Action::Format, 0, 0) == -EROFS);
    assert(action_allowed(state, Action::Mount, 0, 0) == 0);
    state.disks[0].read_only = false;
    state.disks[0].mounted = true;
    assert(action_allowed(state, Action::Create, -1, 0) == -EBUSY);
    assert(action_allowed(state, Action::Delete, 0, 0) == -EBUSY);
    state.disks[0].mounted = false;
    part.info.gpt_type = RELIEFOS_BLOCK_GPT_BIOS_BOOT;
    assert(action_allowed(state, Action::Format, 0, 0) == -EPERM);
    assert(action_allowed(state, Action::Mount, 0, 0) == -ENOTSUP);
    part.info.gpt_type = RELIEFOS_BLOCK_GPT_LINUX;
    part.mount_count = 1;
    strcpy(part.mount_path, "/media/my data");
    assert(action_allowed(state, Action::Format, 0, 0) == -EBUSY);
    assert(action_allowed(state, Action::Mount, 0, 0) == -EBUSY);
    assert(action_allowed(state, Action::Unmount, 0, 0) == 0);
    part.system = true;
    assert(action_allowed(state, Action::Unmount, 0, 0) == -EPERM);
    part.system = false;
    part.mount_count = 2;
    assert(action_allowed(state, Action::Unmount, 0, 0) == -EBUSY);
    part.mount_count = 0;
    part.info.filesystem = RELIEFOS_BLOCK_FILESYSTEM_UNKNOWN;
    assert(action_allowed(state, Action::Mount, 0, 0) == -ENOTSUP);
    state.partition_error = -EIO;
    assert(action_allowed(state, Action::Create, -1, 0) == -EIO);
    state.partition_error = 0;

    Confirmation confirm;
    assert(!confirm.accept(Action::Delete, "DELETE"));
    assert(confirm.accept(Action::Delete, "DELETE"));
    confirm.reset();
    assert(!confirm.accept(Action::Delete, "DELETE"));
    assert(!confirm.accept(Action::Delete, "DELETE "));
    assert(!confirm.accept(Action::Delete, "DELETE"));
    assert(!confirm.accept(Action::Format, "FORMAT"));
    assert(confirm.accept(Action::Format, "FORMAT"));

    char text[64];
    format_size(text, sizeof(text), 1536ULL * 1024 * 1024);
    assert(!strcmp(text, "1.50 GiB"));
    format_size(text, sizeof(text), 512ULL * 1024 * 1024);
    assert(!strcmp(text, "512 MiB"));
    assert(valid_filesystem(RELIEFOS_BLOCK_FILESYSTEM_FAT32));
    assert(valid_filesystem(RELIEFOS_BLOCK_FILESYSTEM_EXT4));
    assert(valid_filesystem(RELIEFOS_BLOCK_FILESYSTEM_EXFAT));
    assert(!valid_filesystem(RELIEFOS_BLOCK_FILESYSTEM_ISO9660));
    puts("PASS Diskmgr: input bounds, exact two-step confirmation, permissions, mounted/system/BIOS/readonly safeguards");
}
