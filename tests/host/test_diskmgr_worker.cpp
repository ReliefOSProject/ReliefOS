#include "worker.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

namespace dm {
/** @brief Exercise real fork/pipe/wait lifetimes without opening a disk. */
void perform_request(const Request &request, Reply &reply)
{
    if (request.size_mib == 2) _exit(17);
    usleep(150000);
    reply.status = -ENOSPC;
    reply.snapshot.disk_count = RELIEFOS_BLOCK_MAX_DISKS;
    reply.snapshot.selected_disk = 0;
    reply.snapshot.partition_count = RELIEFOS_BLOCK_MAX_PARTITIONS;
    strcpy(reply.snapshot.partitions[127].info.name, "中文末尾分区");
}
}

int main()
{
    dm::Worker worker;
    dm::Request request{};
    request.action = dm::Action::Refresh;
    assert(worker.start(request) == 0);
    assert(worker.busy());
    assert(worker.start(request) == -EBUSY);
    assert(worker.poll() == 0); // The caller stays live while child sleeps.
    int complete = 0;
    for (int i = 0; !complete && i < 500; ++i) {
        usleep(10000);
        complete = worker.poll();
    }
    assert(complete == 1 && !worker.busy());
    assert(worker.reply().status == -ENOSPC);
    assert(worker.reply().snapshot.partition_count == 128);
    assert(!strcmp(worker.reply().snapshot.partitions[127].info.name, "中文末尾分区"));
    request.size_mib = 2;
    assert(worker.start(request) == 0);
    complete = 0;
    for (int i = 0; !complete && i < 500; ++i) {
        usleep(10000);
        complete = worker.poll();
    }
    assert(complete == 1 && worker.reply().status == -EIO);
    assert(!worker.busy());
    assert(worker.poll() == 0);
    puts("PASS Diskmgr worker: nonblocking real child, complete large UTF-8 reply, failed-child detection and reaping");
}
