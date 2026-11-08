"""Exercise the locked musl patch against real mutex-attribute source."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MutexProtocolTests(unittest.TestCase):
    def test_built_libc_with_unavailable_pi_syscall(self):
        prefix = Path(os.environ.get("RELIEFOS_TEST_OUTPUT", ROOT / "out/x86_64/release")) / "sysroot/musl"
        stamp = prefix / ".leonos-musl.json"
        if not stamp.is_file() or not any("mutex-protocol" in p["path"]
                                         for p in json.loads(stamp.read_text())["patches"]):
            self.skipTest("build the musl sysroot with the PI capability patch first")
        with tempfile.TemporaryDirectory(prefix="musl-pi-runtime-") as directory:
            work = Path(directory)
            source = work / "probe.c"
            source.write_text('''#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <stdio.h>
struct filter { unsigned short code; unsigned char jt,jf; unsigned k; };
struct program { unsigned short count; struct filter *filters; };
int main(void)
{
    /* An isolated Linux process models ReliefNT's unavailable FUTEX_LOCK_PI.
     * All other syscalls retain their real Linux behavior. */
    struct filter filters[]={
        {0x20,0,0,0}, {0x15,0,3,202}, {0x20,0,0,24},
        {0x15,0,1,6}, {0x06,0,0,0x00050000U|ENOSYS}, {0x06,0,0,0x7fff0000U}};
    struct program program={sizeof(filters)/sizeof(filters[0]),filters};
    assert(!prctl(38,1,0,0,0));
    assert(!prctl(22,2,&program));
    pthread_mutexattr_t attr;pthread_mutex_t mutex;
    assert(!pthread_mutexattr_init(&attr));
    assert(pthread_mutexattr_setprotocol(&attr,PTHREAD_PRIO_INHERIT)==ENOTSUP);
    assert(!pthread_mutex_init(&mutex,&attr));
    assert(!pthread_mutex_lock(&mutex));assert(!pthread_mutex_unlock(&mutex));
    assert(!pthread_mutex_destroy(&mutex));assert(!pthread_mutexattr_destroy(&attr));
    puts("PASS rebuilt musl: missing PI syscall selects a working ordinary mutex");
    return 0;
}
''')
            binary = work / "probe"
            subprocess.run(["clang", "--target=x86_64-linux-musl", f"--sysroot={prefix}",
                            "-fuse-ld=lld", "-nostdlib", "-O2", "-Wall", "-Wextra", "-Werror",
                            prefix / "lib/crt1.o", prefix / "lib/crti.o", source,
                            "-L", prefix / "lib", "-l:libmimalloc.so.3", "-lc", prefix / "lib/crtn.o",
                            "-o", binary], check=True)
            subprocess.run([prefix / "lib/libc.so", "--library-path", prefix / "lib", binary],
                           check=True, timeout=10)

    def test_optional_pi_probe_reports_posix_capability(self):
        with tempfile.TemporaryDirectory(prefix="musl-pi-protocol-") as directory:
            work = Path(directory)
            source = work / "src/thread"
            source.mkdir(parents=True)
            name = "pthread_mutexattr_setprotocol.c"
            shutil.copyfile(ROOT / "third_party/musl/src/thread" / name, source / name)
            lock = json.loads((ROOT / "configs/dependencies.lock.json").read_text())
            musl = next(item for item in lock["dependencies"] if item["id"] == "musl")
            patch = next(item for item in musl["patches"] if "mutex-protocol" in item["path"])
            patch_path = ROOT / patch["path"]
            self.assertEqual(hashlib.sha256(patch_path.read_bytes()).hexdigest(), patch["sha256"])
            subprocess.run(["patch", "--batch", "--forward", "-p1", "-d", work, "-i", patch_path],
                           check=True, capture_output=True)
            (source / "pthread_impl.h").write_text('''#include <errno.h>
typedef struct { unsigned __attr; } pthread_mutexattr_t;
#define PTHREAD_PRIO_NONE 0
#define PTHREAD_PRIO_INHERIT 1
#define PTHREAD_PRIO_PROTECT 2
#define FUTEX_LOCK_PI 6
#define a_store(p, v) (*(p) = (v))
''')
            (source / "syscall.h").write_text('''#define SYS_futex 202
long probe(long, volatile int *, long, long, long);
#define __syscall probe
''')
            harness = work / "test.c"
            harness.write_text('''#include <assert.h>
#include <stdlib.h>
#include "pthread_impl.h"
int test_setprotocol(pthread_mutexattr_t *, int);
static int error, calls;
long probe(long number,volatile int *word,long operation,long value,long timeout)
{assert(number==202 && word && !*word && operation==6 && !value && !timeout);++calls;return -error;}
int main(int argc,char **argv)
{
    assert(argc==3);error=atoi(argv[1]);int expected=atoi(argv[2]);
    pthread_mutexattr_t attr={0x80};
    assert(!test_setprotocol(&attr,PTHREAD_PRIO_NONE) && !calls && attr.__attr==0x80);
    assert(test_setprotocol(&attr,PTHREAD_PRIO_INHERIT)==expected && calls==1);
    assert(attr.__attr==(unsigned)(expected ? 0x80 : 0x88));
    assert(test_setprotocol(&attr,PTHREAD_PRIO_INHERIT)==expected && calls==1);
    assert(test_setprotocol(&attr,PTHREAD_PRIO_PROTECT)==ENOTSUP && calls==1);
    assert(test_setprotocol(&attr,3)==EINVAL && calls==1);
    assert(!test_setprotocol(&attr,PTHREAD_PRIO_NONE) && attr.__attr==0x80);
    return 0;
}
''')
            binary = work / "test"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                            "-fno-pie", "-no-pie", "-Dpthread_mutexattr_setprotocol=test_setprotocol",
                            "-I", source, source / name, harness, "-o", binary], check=True)
            for error, expected in ((38, 95), (95, 95), (0, 0), (22, 22), (14, 14)):
                with self.subTest(kernel_error=error):
                    subprocess.run([binary, str(error), str(expected)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
