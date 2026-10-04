#ifndef RELIEFOS_AUDIO_TEST_H
#define RELIEFOS_AUDIO_TEST_H
#include <stdint.h>
/* Blocking worker/CLI operation, never call from a GUI render loop. Uses a
 * nonblocking PCM and bounded waits. Cancellation returns -1/ECANCELED. */
int reliefos_audio_test_tone(const char *pcm, uint32_t channels, uint32_t seconds,
                            int (*cancelled)(void *), void *context);
#endif
