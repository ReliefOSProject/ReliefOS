#include <assert.h>
#include <stdint.h>
#include <stdio.h>
extern int soundctl_main(int, char **);
extern unsigned fake_writes;
extern int fake_watch;
extern int64_t fake_values[2][7][128];
static int run(int argc, char **argv) { return soundctl_main(argc, argv); }
int main(void) {
    char *cards[] = {"soundctl", "cards"}; assert(run(2, cards) == 0);
    char *list[] = {"soundctl", "--card", "2", "controls"}; assert(run(4, list) == 0);
    char *missing[] = {"soundctl", "get", "No such control"}; assert(run(3, missing) == 1);
    char *badcard[] = {"soundctl", "--card", "99", "controls"}; assert(run(4, badcard) == 1);
    char *percent[] = {"soundctl", "set", "Master Playback Volume", "51%"};
    assert(run(4, percent) == 0 && fake_values[0][0][0] == 20 && fake_values[0][0][1] == 20);
    char *channels[] = {"soundctl", "set", "1", "10,30"};
    assert(run(4, channels) == 0 && fake_values[0][0][0] == 10 && fake_values[0][0][1] == 30);
    char *othercard[] = {"soundctl", "--card", "2", "set", "1", "30"};
    assert(run(6, othercard) == 0 && fake_values[1][0][0] == 30 && fake_values[0][0][0] == 10);
    char *route[] = {"soundctl", "route", "headphones"};
    assert(run(3, route) == 0 && fake_values[0][2][0] == 0); /* Reversed enum! */
    char *source[] = {"soundctl", "capture-source", "microphone"};
    assert(run(3, source) == 0 && fake_values[0][3][0] == 1);
    char *get[] = {"soundctl", "get", "Output Source"}; assert(run(3, get) == 0);
    unsigned writes = fake_writes;
    char *invalid[] = {"soundctl", "set", "Master Playback Volume", "101%"}; assert(run(4, invalid) == 1);
    char *step[] = {"soundctl", "set", "1", "19"}; assert(run(4, step) == 1);
    char *jack[] = {"soundctl", "set", "Headphone Jack", "on"}; assert(run(4, jack) == 1);
    char *enum_bad[] = {"soundctl", "route", "Unknown"}; assert(run(3, enum_bad) == 1);
    char *trailing[] = {"soundctl", "get", "1", "unexpected"}; assert(run(4, trailing) == 2);
    assert(fake_writes == writes);
    fake_watch = 1;
    char *watch[] = {"soundctl", "watch"}; assert(run(2, watch) == 0);
    fake_watch = 0;
    puts("soundctl exact names, percent/step, channels, actual enum labels, cards, errors and exit codes PASS");
    return 0;
}
