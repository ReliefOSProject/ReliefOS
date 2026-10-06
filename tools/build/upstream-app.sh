#!/bin/sh
# ReliefOS adapters for upstream C programs. No upstream source is modified.
set -eu
MAKEFLAGS=${MAKEFLAGS-}
case ${MAKEFLAGS%% *} in *n*) exit 0 ;; esac
[ "$#" = 12 ] || { echo 'usage: upstream-app.sh PACKAGE SRC WORK OUTPUT MUSL RUNTIME ARCHIVE INCLUDE AUTOCONF CC LD AR' >&2; exit 2; }
pkg=$1 src=$2 work=$3 output=$4 musl=$5 runtime=$6 archive=$7 includes=$8 autoconf=$9
shift 9
cc=$1 ld=$2 ar=$3
resource=$("$cc" -print-resource-dir)
mkdir -p "$work" "$output"
rm -rf "$work/objects" "$work/generated"
mkdir -p "$work/objects" "$work/generated"
source=$src/third_party/$pkg
port=$src/userland/$pkg
source_id=$pkg
case $pkg in pleditor) source_id=pl_editor ;; esac
if [ -n "${UPSTREAM_DEPS-}" ]; then
 expected=$("$UPSTREAM_DEPS" --lock "$src/configs/dependencies.lock.json" --id "$source_id" --print commit)
 [ "$(git -C "$src/third_party/$source_id" rev-parse HEAD)" = "$expected" ] || { echo "$pkg: source revision mismatch" >&2; exit 1; }
fi
flags="--target=x86_64-linux-musl ${UPSTREAM_CFLAGS:--O2} -std=gnu11 -ffreestanding -fno-stack-protector -fPIC -ffunction-sections -fdata-sections -nostdinc -isystem $resource/include -I$musl/include -I$src/userland/runtime/include -I$src/include -I${UPSTREAM_UAPI:-$src/kernel/reliefnt/include/uapi} -I$includes -I$work/generated -I$source -DRELIEFOS_USE_MUSL -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L"
compile() { "$cc" $flags "$@"; }
shared() { name=$1; shift; "$ld" ${UPSTREAM_LDFLAGS-} -shared --no-undefined --hash-style=both -z max-page-size=0x1000 -soname "$name" -o "$output/$name.tmp" "$@" -L "$musl/lib" -l:libmimalloc.so.3 "$runtime" -lc; mv "$output/$name.tmp" "$output/$name"; }
staticlib() { name=$1; shift; rm -f "$output/$name.tmp"; "$ar" rcs "$output/$name.tmp" "$@"; mv "$output/$name.tmp" "$output/$name"; }
executable() { "$ld" ${UPSTREAM_LDFLAGS-} --gc-sections -z max-page-size=0x1000 -pie --hash-style=both --dynamic-linker /lib/ld-musl-x86_64.so.1 -rpath /usr/lib/reliefos:/lib:/usr/lib -o "$output/$pkg.elf.tmp" "$musl/lib/Scrt1.o" "$musl/lib/crti.o" "$@" -L "$musl/lib" -l:libmimalloc.so.3 --start-group "$runtime" -lc --end-group "$musl/lib/crtn.o"; mv "$output/$pkg.elf.tmp" "$output/$pkg.elf"; }
staticexe() { "$ld" ${UPSTREAM_LDFLAGS-} --gc-sections -z max-page-size=0x1000 -static --image-base=0x4000000 -o "$output/$pkg.elf.tmp" "$musl/lib/crt1.o" "$musl/lib/crti.o" "$@" "$musl/lib/mimalloc.o" --start-group "$archive" "$musl/lib/libc.a" --end-group "$musl/lib/crtn.o"; mv "$output/$pkg.elf.tmp" "$output/$pkg.elf"; }
case $pkg in
pleditor)
 source=$src/third_party/pl_editor
 cp -R "$source/src" "$work/generated/src"
 flags="$flags -I$work/generated/src"
 awk '
 /#include <stdlib.h>/ {print; print "#include <stdint.h>"; next}
 /    size_t buffer_size = \(size_t\)\(state->screen_rows \+ 2\) \*/ {
   print "    size_t rows = (size_t)state->screen_rows;"
   print "    size_t cols = (size_t)state->screen_cols;"
   print "    if (state->screen_rows < 0 || state->screen_cols < 0 || rows > SIZE_MAX - 2U || cols > (SIZE_MAX - 32U) / 16U) return;"
   print "    size_t bytes_per_row = cols * 16U + 32U;"
   print "    rows += 2U;"
   print "    if (rows > (SIZE_MAX - 256U) / bytes_per_row) return;"
   print "    size_t buffer_capacity = rows * bytes_per_row + 256U;"
   print "    char *buffer = malloc(buffer_capacity);"
   getline; getline; changed++; next
 }
 {print}
 END {if (changed != 1) exit 1}
 ' "$source/src/pleditor.c" > "$work/generated/src/pleditor.c"
 for name in main pleditor syntax; do compile -include "$autoconf" -fPIE -c "$work/generated/src/$name.c" -o "$work/objects/$name.o"; done
 compile -include "$autoconf" -fPIE -c "$src/userland/apps/pleditor/platform_reliefos.c" -o "$work/objects/platform.o"
 executable "$work"/objects/*.o
 ;;
sl)
 compile -D_DEFAULT_SOURCE -fPIE -c "$source/sl.c" -o "$work/objects/sl.o"
 executable "$work/objects/sl.o"
 ;;
sqlite)
 sed "s@TOP = ../sqlite@TOP = $source@" "$source/Makefile.linux-gcc" > "$work/generated/Makefile"
 (cd "$work/generated" && make sqlite3.c sqlite3.h)
 flags="$flags -DSQLITE_OS_OTHER=1 -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION=1 -DSQLITE_OMIT_WAL=1 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_MAX_MMAP_SIZE=0"
 compile -c "$work/generated/sqlite3.c" -o "$work/objects/sqlite3.o"
 compile -c "$port/leonos_sqlite_vfs.c" -o "$work/objects/vfs.o"
 shared sqlite.so.3 "$work"/objects/*.o
 staticlib libsqlite3.a "$work"/objects/*.o
 cp "$work/generated/sqlite3.h" "$output/sqlite3.h"
 ;;
*) echo "unsupported upstream app: $pkg" >&2; exit 2 ;;
esac
