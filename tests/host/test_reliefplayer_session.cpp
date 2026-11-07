#include "../../userland/apps/reliefplayer/session.h"
#include <assert.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
static void finish(rp::Session &session) {
  session.shutdown();
  auto end = rp::monotonic_ms() + 3000;
  while (session.active() && rp::monotonic_ms() < end) {
    session.poll();
    usleep(5000);
  }
  assert(!session.active());
}
int main(int argc, char **argv) {
  assert(argc == 2);
  rp::Session session;
  assert(!session.open("", 70));
  assert(session.open("/no-such-reliefplayer-file", 70));
  auto end = rp::monotonic_ms() + 3000;
  while (session.status().state != rp::State::Failed &&
         rp::monotonic_ms() < end) {
    session.poll();
    usleep(5000);
  }
  assert(session.status().state == rp::State::Failed &&
         session.status().error == rp::Error::Io);
  finish(session);
  for (unsigned n = 0; n < 12; ++n) {
    assert(session.open(argv[1], 70));
    assert(session.command(rp::Op::Volume, 0));
    assert(session.open("/no-such-reliefplayer-file", 25));
    end = rp::monotonic_ms() + 4000;
    while (session.status().state != rp::State::Failed &&
           rp::monotonic_ms() < end) {
      session.poll();
      usleep(5000);
    }
    assert(session.status().state == rp::State::Failed &&
           session.status().error == rp::Error::Io);
    finish(session);
  }
  assert(session.open(argv[1], 70));
  finish(session);
  // All waits are for our children; no worker may remain as a zombie.
  assert(waitpid(-1, nullptr, WNOHANG) == -1);
  puts("ReliefPlayer: worker errors, cancellation, replacement and reaping "
       "passed");
}
