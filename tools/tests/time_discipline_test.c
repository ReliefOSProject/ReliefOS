#include <assert.h>
#include <stdio.h>
#include "../../kernel/reliefnt/kernel/time/time_discipline.c"
int main(void)
{
    struct linux_timespec now={.tv_sec=1000};
    struct linux_timex tx={0};
    assert(time_discipline_adjust(&tx,&now,false)==TIME_ERROR);
    assert(tx.status & STA_UNSYNC);
    tx=(struct linux_timex){.modes=ADJ_FREQUENCY,.freq=65536};
    assert(time_discipline_adjust(&tx,&now,false)==-LINUX_EPERM);
    assert(time_discipline_adjust(&tx,&now,true)==TIME_ERROR);
    uint64_t total=0;
    for(unsigned i=0;i<100;i++) total+=time_discipline_tick_ns();
    assert(total==1000001000ULL); /* 1 ppm really changes the clock. */
    tx=(struct linux_timex){.modes=ADJ_FREQUENCY,.freq=-65536};
    assert(time_discipline_adjust(&tx,&now,true)==TIME_ERROR);
    total=0; for(unsigned i=0;i<100;i++) total+=time_discipline_tick_ns();
    assert(total==999999000ULL);
    tx=(struct linux_timex){.modes=ADJ_TICK,.tick=8999};
    assert(time_discipline_adjust(&tx,&now,true)==-LINUX_EINVAL);
    tx=(struct linux_timex){.modes=ADJ_STATUS|ADJ_MAXERROR|ADJ_FREQUENCY|ADJ_TIMECONST|ADJ_NANO,
        .status=STA_PLL|STA_FREQHOLD,.constant=0};
    assert(time_discipline_adjust(&tx,&now,true)==TIME_OK);
    tx=(struct linux_timex){.modes=ADJ_OFFSET,.offset=4000000};
    assert(time_discipline_adjust(&tx,&now,true)==TIME_OK);
    assert(tx.offset==4000000);
    assert(time_discipline_second(1001)==0);
    total=0; for(unsigned i=0;i<100;i++) total+=time_discipline_tick_ns();
    assert(total==1001000000ULL); /* first second removes 1/4 phase error */
    tx=(struct linux_timex){0};
    assert(time_discipline_adjust(&tx,&now,false)==TIME_OK && tx.offset==3000000);
    time_discipline_clear();
    tx=(struct linux_timex){.modes=ADJ_OFFSET_SINGLESHOT,.offset=-1000};
    assert(time_discipline_adjust(&tx,&now,true)==TIME_ERROR);
    assert(time_discipline_second(1002)==0);
    total=0; for(unsigned i=0;i<100;i++) total+=time_discipline_tick_ns();
    assert(total==999500000ULL);
    tx=(struct linux_timex){.modes=ADJ_OFFSET_SS_READ};
    assert(time_discipline_adjust(&tx,&now,false)==TIME_ERROR && tx.offset==-500);
    time_discipline_clear();
    tx=(struct linux_timex){.modes=ADJ_STATUS|ADJ_MAXERROR,.status=STA_PLL|STA_INS};
    assert(time_discipline_adjust(&tx,&now,true)==TIME_OK);
    assert(time_discipline_second(86399)==0);
    assert(time_discipline_second(86400)==-1 && time_discipline_tai()==1);
    assert(time_discipline_second(86400)==0);
    tx=(struct linux_timex){.modes=ADJ_STATUS|ADJ_MAXERROR,.status=STA_PLL};
    assert(time_discipline_adjust(&tx,&now,true)==TIME_WAIT);
    assert(time_discipline_second(86401)==0);
    tx=(struct linux_timex){.modes=ADJ_FREQUENCY,.freq=INT64_MAX};
    assert(time_discipline_adjust(&tx,&now,true)==TIME_OK && tx.freq==32768000);
    tx=(struct linux_timex){.modes=ADJ_OFFSET,.offset=INT64_MIN};
    assert(time_discipline_adjust(&tx,&now,true)==TIME_OK && tx.offset==-500000000);
    puts("PASS NTP actual frequency/phase slew, permissions, adjtime, limits and leap insertion");
}
