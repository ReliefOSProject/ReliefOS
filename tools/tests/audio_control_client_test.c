#include <reliefos/audio_control.h>
#include <alsa/asoundlib.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern unsigned fake_reads, fake_writes, fake_polls, fake_closes, fake_channels;
extern int fake_error, fake_poll_error, fake_nonblock;
extern int64_t fake_values[2][7][128];
extern void fake_external_event(struct reliefos_audio_control *, unsigned);
static void interrupted(int sig) { (void)sig; }
int main(int argc, char **argv) {
    assert(argc == 2);
    struct reliefos_audio_control *c = NULL;
    assert(reliefos_audio_control_open(0, &c) == 0 && c);
    int64_t values[130] = { 999, 18 }; uint32_t count, id, mask;
    if (!strcmp(argv[1], "range")) {
        assert(reliefos_audio_control_set(c, 1, values, 1) == -1 && errno == EINVAL && fake_writes == 0);
        values[0] = 20;
        assert(reliefos_audio_control_set(c, 1, values, 1) == 0 && fake_writes == 1);
        assert(fake_values[0][0][0] == 20 && fake_values[0][0][1] == 20);
        assert(reliefos_audio_control_get(c, 1, values, 2) == 0 && fake_reads == 1 && values[1] == 20);
        values[0] = 19; assert(reliefos_audio_control_set(c, 1, values, 1) == -1 && errno == EINVAL);
        struct reliefos_audio_control_desc desc[7];
        assert(reliefos_audio_control_list(c, NULL, 0, &count) == -1 && errno == ERANGE && count == 7);
        assert(reliefos_audio_control_list(c, desc, 7, &count) == 0 && count == 7);
        assert(desc[0].minimum == 0 && desc[0].maximum == 40 && desc[0].step == 2 && desc[2].items == 2);
    } else if (!strcmp(argv[1], "shape")) {
        fake_channels = 4; values[0] = 20;
        assert(reliefos_audio_control_set(c, 1, values, 2) == -1 && errno == ERANGE && fake_writes == 0);
        fake_channels = 129;
        assert(reliefos_audio_control_get(c, 1, values, 130) == -1 && errno == EOVERFLOW && fake_reads == 0);
        fake_channels = 2;
        assert(reliefos_audio_control_get(c, 7, values, 130) == -1 && errno == ENOTSUP && fake_reads == 0);
        values[0] = INT64_MAX;
        assert(reliefos_audio_control_set(c, 6, values, 1) == 0);
    } else if (!strcmp(argv[1], "errors")) {
        assert(reliefos_audio_control_get(c, 1, NULL, 0) == -1 && errno == EINVAL);
        assert(reliefos_audio_control_list(c, NULL, 0, NULL) == -1 && errno == EINVAL);
        values[0] = 1; assert(reliefos_audio_control_set(c, 5, values, 1) == -1 && errno == EROFS && fake_writes == 0);
        values[0] = 2; assert(reliefos_audio_control_set(c, 3, values, 1) == -1 && errno == EINVAL);
        fake_error = EACCES;
        assert(reliefos_audio_control_get(c, 1, values, 2) == -1 && errno == EACCES);
        struct reliefos_audio_control *missing = c;
        assert(reliefos_audio_control_open(99, &missing) == -1 && errno == ENOENT && !missing);
        fake_error = 0;
    } else if (!strcmp(argv[1], "events")) {
        assert(fake_nonblock);
        assert(reliefos_audio_control_wait(c, 0, &id, &mask) == 0 && id == 0 && mask == 0);
        fake_external_event(c, 1);
        assert(reliefos_audio_control_wait(c, 100, &id, &mask) == 1 && id == 1 && mask == SND_CTL_EVENT_MASK_VALUE);
        assert(reliefos_audio_control_get(c, 1, values, 2) == 0 && values[0] == 12 && values[1] == 14 && fake_polls == 2);
        fake_poll_error = EACCES;
        assert(reliefos_audio_control_wait(c, 0, &id, &mask) == -1 && errno == EACCES);
        fake_poll_error = 0;
        struct sigaction action = {0}; action.sa_handler = interrupted; sigaction(SIGALRM, &action, NULL);
        alarm(1); assert(reliefos_audio_control_wait(c, 3000, &id, &mask) == -1 && errno == EINTR); alarm(0);
    } else if (!strcmp(argv[1], "close")) {
        reliefos_audio_control_close(c); reliefos_audio_control_close(c);
        assert(fake_closes == 1);
        assert(reliefos_audio_control_get(c, 1, values, 2) == -1 && errno == EINVAL);
        c = NULL;
    } else assert(0);
    reliefos_audio_control_close(c);
    printf("audio control %s PASS (read=%u write=%u poll=%u close=%u)\n", argv[1], fake_reads, fake_writes, fake_polls, fake_closes);
    return 0;
}
