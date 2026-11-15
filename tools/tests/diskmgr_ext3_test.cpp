/* SPDX-License-Identifier: Apache-2.0 */
#include "../../userland/apps/diskmgr/model.h"
#include <cassert>
#include <cerrno>
#include <cstdio>

/** @brief Check ext3 admission preserves the existing device safety gates.
 * @return Zero on success.
 */
int main()
{
    assert(dm::valid_filesystem(RELIEFOS_BLOCK_FILESYSTEM_EXT3));
    dm::Snapshot snapshot{};
    snapshot.disk_count=1; snapshot.partition_count=1; snapshot.selected_disk=0;
    snapshot.disks[0].info.sector_size=512;
    snapshot.partitions[0].info.gpt_type=RELIEFOS_BLOCK_GPT_LINUX;
    snapshot.partitions[0].info.filesystem=RELIEFOS_BLOCK_FILESYSTEM_EXT3;
    assert(dm::action_allowed(snapshot,dm::Action::Mount,0,0)==0);
    assert(dm::action_allowed(snapshot,dm::Action::Format,0,0)==0);
    assert(dm::action_allowed(snapshot,dm::Action::Format,0,1000)==-EACCES);
    snapshot.partitions[0].system=true;
    assert(dm::action_allowed(snapshot,dm::Action::Format,0,0)==-EPERM);
    std::puts("PASS diskmgr ext3 admission and safety gates");
    return 0;
}
