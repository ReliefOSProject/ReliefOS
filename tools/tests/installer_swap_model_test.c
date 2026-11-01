/* Real installer capacity calculation, with hand-derived swap boundaries. */
#include "../../userland/apps/installer/installer_capacity.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

extern uint64_t installer_swap_recommend_mib(uint64_t) __attribute__((weak));
extern int installer_swap_size_valid(uint64_t) __attribute__((weak));
extern int installer_capacity_add_swap(struct installer_capacity *, uint64_t) __attribute__((weak));
extern int installer_swap_parse_mib(const char *,uint64_t *) __attribute__((weak));

int main(void)
{
    if (!installer_swap_recommend_mib || !installer_swap_size_valid || !installer_capacity_add_swap ||
        !installer_swap_parse_mib) {
        fputs("FAIL: installer swap recommendation, validation and capacity are absent\n", stderr);
        return 1;
    }
    assert(installer_swap_recommend_mib(0)==512);
    assert(installer_swap_recommend_mib(512*1024)==512);
    assert(installer_swap_recommend_mib(1024*1024+1)==1025);
    assert(installer_swap_recommend_mib(UINT64_MAX)==2048);
    assert(!installer_swap_size_valid(0));
    assert(installer_swap_size_valid(1));
    assert(installer_swap_size_valid(2049));
    assert(installer_swap_size_valid(16777216));
    assert(!installer_swap_size_valid(16777217));
    assert(!installer_swap_size_valid(UINT64_MAX));
    uint64_t size=99;
    assert(!installer_swap_parse_mib("2049",&size) && size==2049);
    assert(installer_swap_parse_mib("0",&size)<0 && size==2049);
    assert(installer_swap_parse_mib("18446744073709551616",&size)<0 && size==2049);
    assert(installer_swap_parse_mib("-1",&size)<0 && size==2049);
    assert(installer_swap_parse_mib("512x",&size)<0 && size==2049);
    struct installer_capacity need={.root_bytes=256ULL*1024*1024,.root_inodes=3};
    assert(!installer_capacity_add_swap(&need,1024));
    assert(need.root_bytes==1280ULL*1024*1024 && need.root_inodes==4);
    assert(installer_capacity_check(&need,1024ULL*2048,128*2048)==-ENOSPC);
    assert(!installer_capacity_check(&need,2048ULL*2048,128*2048));
    need.root_bytes=UINT64_MAX-512; need.root_inodes=3;
    assert(installer_capacity_add_swap(&need,1)==-EOVERFLOW);
    assert(need.root_bytes==UINT64_MAX-512 && need.root_inodes==3);
    puts("PASS swap recommendation, editable limits, checked capacity and spare");
    return 0;
}
