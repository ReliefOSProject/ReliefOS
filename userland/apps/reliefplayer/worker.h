#ifndef RELIEFPLAYER_WORKER_H
#define RELIEFPLAYER_WORKER_H
namespace rp {
struct VideoShared;
int worker(const char *path, int commands, int statuses, unsigned volume,
           VideoShared *video = nullptr);
}
#endif
