#!/bin/sh
# Offline adapter tests. Python is used only to construct hostile test archives.
set -eu
LC_ALL=C
export LC_ALL
repo=$(CDPATH='' cd -- "$(dirname "$0")/../.." && pwd -P)
tmp_root=${TMPDIR:-$repo/out/tmp}
mkdir -p "$tmp_root"
work=$(mktemp -d "$tmp_root/reliefos-libbsd-development.XXXXXX")
trap 'rm -rf "$work"' 0
trap 'exit 130' HUP INT TERM
fail() { printf 'libbsd-development test: %s\n' "$*" >&2; exit 1; }
adapter=$repo/tools/build/libbsd-development.sh
src=$work/source
cache=$work/cache
output=$work/out\ with\ spaces/libbsd/root
mkdir -p "$work/bin" "$work/good" "$cache" "$work/digests" \
    "$src/resources/licenses" "$src/system/rootfs/etc/apk/keys" "$work/md/lib"
cp "$repo/resources/licenses/libbsd-LICENSE" "$src/resources/licenses/libbsd-LICENSE"
printf 'fixture lock\n' >"$work/lock"
export FIXTURE_WORK="$work" FIXTURE_SRC="$src"
real_mv=$(command -v mv)
real_tar=$(command -v tar)
real_find=$(command -v find)
export FIXTURE_MV="$real_mv" FIXTURE_TAR="$real_tar" FIXTURE_FIND="$real_find"

cat >"$work/bin/deps" <<'DATA'
#!/bin/sh
set -eu
[ "$#" = 6 ] && [ "$1" = --lock ] && [ "$2" = "$FIXTURE_WORK/lock" ] &&
    [ "$3" = --id ] && [ "$5" = --print ] || exit 2
case $4 in
    alpine-libbsd) name=libbsd;;
    runtime-dev-libbsd-dev) name=libbsd-dev;;
    runtime-dev-libbsd-static) name=libbsd-static;;
    *) exit 2;;
esac
case $6 in
    url)
        if [ "${BAD_URL:-0}" = 1 ]; then printf 'https://fixture.invalid/../bad.apk\n'; exit; fi
        printf 'https://dl-cdn.alpinelinux.org/alpine/v3.24/main/x86_64/%s-0.12.2-r0.apk\n' "$name";;
    sha256)
        if [ "${BAD_DIGEST:-0}" = 1 ]; then printf 'bad-digest\n'; exit; fi
        cat "$FIXTURE_WORK/digests/$name";;
    *) exit 2;;
esac
DATA
cat >"$work/bin/apk" <<'DATA'
#!/bin/sh
set -eu
[ "$#" = 4 ] && [ "$1" = --keys-dir ] &&
    [ "$2" = "$FIXTURE_SRC/system/rootfs/etc/apk/keys" ] && [ "$3" = verify ] || exit 2
sha256sum "$4" >>"$FIXTURE_WORK/verifications"
if [ "${FAIL_VERIFY_AFTER:-0}" = "$(wc -l <"$FIXTURE_WORK/verifications" | tr -d ' ')" ]; then exit 9; fi
DATA
cat >"$work/bin/mv" <<'DATA'
#!/bin/sh
set -eu
if [ "$#" = 2 ] && [ "$2" = "${FAIL_PUBLISH_DEST:-}" ] && [ -f "$FIXTURE_WORK/fail-once" ]; then
    rm "$FIXTURE_WORK/fail-once"
    exit 9
fi
exec "$FIXTURE_MV" "$@"
DATA
cat >"$work/bin/tar" <<'DATA'
#!/bin/sh
set -eu
if [ "${FAIL_EXTRACT:-0}" = 1 ]; then
    for arg do [ "$arg" != -xOzf ] || exit 9; done
fi
exec "$FIXTURE_TAR" "$@"
DATA
cat >"$work/bin/find" <<'DATA'
#!/bin/sh
set -eu
[ "${FAIL_INVENTORY:-0}" != 1 ] || exit 9
exec "$FIXTURE_FIND" "$@"
DATA
chmod +x "$work/bin/"*

# The static BSD fixture deliberately needs libmd from a separate local stage.
cc=${CC:-cc}
printf 'int fixture_md(void) { return 17; }\n' >"$work/md.c"
printf 'extern int fixture_md(void); int fixture_bsd(void) { return fixture_md(); }\n' >"$work/bsd.c"
"$cc" -fPIC -c "$work/md.c" -o "$work/md.o"
"$cc" -fPIC -c "$work/bsd.c" -o "$work/bsd.o"
ar rcs "$work/md/lib/libmd.a" "$work/md.o"
"$cc" -shared -Wl,-soname,libmd.so.0 "$work/md.o" -o "$work/md/lib/libmd.so.0.0.0"
ln -s libmd.so.0.0.0 "$work/md/lib/libmd.so.0"
ln -s libmd.so.0 "$work/md/lib/libmd.so"
"$cc" -shared -Wl,-soname,libbsd.so.0 "$work/bsd.o" -L"$work/md/lib" -lmd -o "$work/libbsd.so.0.12.2"
ar rcs "$work/libbsd.a" "$work/bsd.o"

cat >"$work/packages.py" <<'PY'
import io
import pathlib
import sys
import tarfile

work = pathlib.Path(sys.argv[1])
script = b'''/* GNU ld script
 * The MD5 functions are provided by the libmd library. */
OUTPUT_FORMAT(elf64-x86-64)
GROUP(/usr/lib/libbsd.so.0.12.2 AS_NEEDED(-lmd))
'''
pc = b'''prefix=/usr
exec_prefix=${prefix}
libdir=${exec_prefix}/lib
includedir=${prefix}/include

Name: libbsd
Description: BSD fixture
Version: 0.12.2
Libs: -L${libdir} -lbsd
Libs.private: -pthread -lmd
Cflags: -I${includedir}
'''
def regular(name, data):
    info = tarfile.TarInfo(name)
    info.size = len(data)
    info.mode = 0o644
    return info, data

for name in ('libbsd', 'libbsd-dev', 'libbsd-static'):
    members = [regular('.PKGINFO', f'pkgname = {name}\npkgver = 0.12.2-r0\narch = x86_64\n'.encode())]
    if name == 'libbsd':
        members += [regular('usr/lib/libbsd.so.0.12.2', (work / 'libbsd.so.0.12.2').read_bytes())]
        link = tarfile.TarInfo('usr/lib/libbsd.so.0')
        link.type = tarfile.SYMTYPE
        link.linkname = 'libbsd.so.0.12.2'
        members.append((link, b''))
    elif name == 'libbsd-dev':
        for header in ('stdlib.h', 'string.h', 'unistd.h', 'sys/queue.h', 'sys/tree.h', 'nested/extra.h'):
            members.append(regular('usr/include/bsd/' + header, b'int fixture_bsd(void);\n'))
        members += [regular('usr/lib/libbsd.so', script)]
        for filename in ('libbsd.pc', 'libbsd-overlay.pc'):
            members.append(regular('usr/lib/pkgconfig/' + filename, pc))
        # These unrelated headers/libraries must never reach the build root.
        for path in ('usr/include/stdlib.h', 'usr/include/sys/cdefs.h', 'usr/include/linux/types.h',
                     'usr/lib/libc.so', 'usr/lib/libmd.a', 'usr/lib/libbsd.la'):
            members.append(regular(path, b'not a libbsd product\n'))
    else:
        members += [regular('usr/lib/libbsd.a', (work / 'libbsd.a').read_bytes())]
    with tarfile.open(work / 'good' / f'{name}-0.12.2-r0.apk', 'w:gz', format=tarfile.USTAR_FORMAT) as archive:
        for info, data in members:
            archive.addfile(info, io.BytesIO(data) if info.isreg() else None)
PY
python3 "$work/packages.py" "$work"

reseal() {
    for name in libbsd libbsd-dev libbsd-static; do
        sum=$(sha256sum "$cache/$name-0.12.2-r0.apk")
        printf '%s\n' "${sum%% *}" >"$work/digests/$name"
    done
}
reset_cache() {
    cp "$work/good/"*.apk "$cache/"
    reseal
    : >"$work/verifications"
}
invoke() {
    PATH="$work/bin:$PATH" sh "$adapter" "$src" "$work/bin/deps" "$work/lock" \
        "$cache" "$work/bin/apk" "$output"
}
snapshot() {
    (cd "$output" && "$real_find" . -printf '%y %p %l\n' &&
        "$real_find" . -type f -exec sha256sum {} \;) | sort
}
check_preserved() {
    snapshot >"$work/after"
    cmp "$work/before" "$work/after" || fail 'failure changed the old root'
    cmp "$work/before.files" "$output.files" || fail 'failure changed the old manifest'
    [ -z "$("$real_find" "$(dirname "$output")" -maxdepth 1 -name '.root.new.*' -print)" ] || fail 'transaction leaked'
}
expect_failure() {
    label=$1
    if invoke >"$work/failure.log" 2>&1; then fail "accepted $label"; fi
    check_preserved
}

reset_cache
invoke
[ "$(wc -l <"$work/verifications" | tr -d ' ')" = 3 ] || fail 'did not verify every package'
cmp "$src/resources/licenses/libbsd-LICENSE" "$output/usr/share/licenses/libbsd/LICENSE"
for library in libbsd.so libbsd.so.0 libbsd.so.0.12.2 libbsd.a; do
    [ "$(readlink "$output/lib/$library")" = "../usr/lib/$library" ] || fail "bad compatibility link: $library"
    test -s "$output/lib/$library"
done
[ "$(readlink "$output/usr/lib/libbsd.so.0")" = libbsd.so.0.12.2 ] || fail 'bad SONAME link'
[ ! -e "$output/lib/libbsd-ctor.a" ] || fail 'invented a ctor archive'
for path in usr/include/stdlib.h usr/include/sys/cdefs.h usr/include/linux/types.h usr/lib/libc.so usr/lib/libmd.a usr/lib/libbsd.la; do
    [ ! -e "$output/$path" ] || fail "selected unrelated input: $path"
done
(cd "$output" && "$real_find" . ! -type d -print | sed 's@^./@@' | sort) >"$work/expected.files"
cmp "$work/expected.files" "$output.files"
grep -Fxq 'usr/include/bsd/nested/extra.h' "$output.files"
grep -Fxq '.complete' "$output.files"
if grep -F '/usr/lib/libbsd.so.0.12.2' "$output/usr/lib/libbsd.so"; then fail 'absolute linker-script path'; fi

# Copy into shadow-style deps, merge the local libmd stage and move the tree.
combined=$work/deps\ before\ moving
relocated=$work/relocated
mkdir -p "$combined"
cp -a "$output/." "$combined/"
cp -a "$work/md/." "$combined/"
mv "$combined" "$relocated"
cat >"$work/consumer.c" <<'DATA'
#include <bsd/stdlib.h>
int main(void) { return fixture_bsd() != 17; }
DATA
"$cc" -I"$relocated/usr/include" "$work/consumer.c" -L"$relocated/lib" -L"$relocated/usr/lib" \
    -Wl,-rpath-link,"$relocated/lib" -lbsd -o "$work/dynamic"
LD_LIBRARY_PATH="$relocated/lib:$relocated/usr/lib" "$work/dynamic"
"$cc" -I"$relocated/usr/include" "$work/consumer.c" -L"$relocated/lib" -L"$relocated/usr/lib" \
    -Wl,-Bstatic -lbsd -lmd -Wl,-Bdynamic -o "$work/static"
"$work/static"
if "$cc" -I"$relocated/usr/include" "$work/consumer.c" -L"$relocated/lib" -L"$relocated/usr/lib" \
    -Wl,-Bstatic -lbsd -Wl,-Bdynamic -o "$work/no-md" >"$work/no-md.log" 2>&1; then
    fail 'static fixture did not require local libmd.a'
fi
for package in libbsd libbsd-overlay; do
    pc_flags=$(PKG_CONFIG_PATH='' PKG_CONFIG_LIBDIR="$relocated/usr/lib/pkgconfig" \
        PKG_CONFIG_SYSROOT_DIR="$relocated" pkg-config --cflags --libs --static "$package")
    case $pc_flags in *"$relocated/usr/"*) ;; *) fail 'pkg-config lost relocated prefix';; esac
    case $pc_flags in *"$output"*|*"$combined"*) fail 'pkg-config retained an old path';; esac
done

snapshot >"$work/before"
cp "$output.files" "$work/before.files"
reset_cache
printf 'corruption\n' >>"$cache/libbsd-0.12.2-r0.apk"
expect_failure 'checksum mismatch'
[ ! -s "$work/verifications" ] || fail 'verification ran before checksum validation'
reset_cache
FAIL_VERIFY_AFTER=3 expect_failure 'invalid signature in final package'
reset_cache
rm "$cache/libbsd-static-0.12.2-r0.apk"
expect_failure 'missing offline cache'
reset_cache
BAD_URL=1 expect_failure 'unsafe locked URL'
BAD_DIGEST=1 expect_failure 'invalid locked digest'
FAIL_EXTRACT=1 expect_failure 'extraction failure'
FAIL_INVENTORY=1 expect_failure 'inventory generation failure'
for destination in "$output" "$output.files"; do
    : >"$work/fail-once"
    FAIL_PUBLISH_DEST=$destination expect_failure 'publication failure'
done

cat >"$work/mutate.py" <<'PY'
import io
import pathlib
import sys
import tarfile

work = pathlib.Path(sys.argv[1])
mode = sys.argv[2]
name = 'libbsd-dev-0.12.2-r0.apk'
with tarfile.open(work / 'good' / name) as archive:
    members = [(info, archive.extractfile(info).read() if info.isreg() else b'') for info in archive]
if mode == 'missing-header':
    members = [(i, d) for i, d in members if i.name != 'usr/include/bsd/stdlib.h']
elif mode == 'metadata':
    for info, data in members:
        if info.name == '.PKGINFO':
            info.size = len(data.replace(b'0.12.2-r0', b'0.12.1-r0'))
    members = [(i, d.replace(b'0.12.2-r0', b'0.12.1-r0') if i.name == '.PKGINFO' else d) for i, d in members]
elif mode == 'script':
    members = [(i, d.replace(b'/usr/lib/', b'/tmp/bad/')) for i, d in members]
else:
    paths = {
        'absolute': str(work / 'escaped'),
        'traversal': 'usr/include/bsd/../../../escaped',
        'dot': 'usr/include/bsd/./extra.h',
        'duplicate': 'usr/include/bsd/stdlib.h',
        'newline': 'usr/include/bsd/new\nline.h',
        'backslash': 'usr/include/bsd/back\\slash.h',
    }
    info = tarfile.TarInfo(paths.get(mode, 'usr/include/bsd/evil.h'))
    data = b'bad input\n'
    if mode in ('absolute-link', 'parent-link', 'dangling-link'):
        info.type = tarfile.SYMTYPE
        info.linkname = {'absolute-link': str(work / 'escaped'), 'parent-link': '../../../../escaped', 'dangling-link': 'missing.h'}[mode]
        data = b''
    elif mode == 'hardlink':
        info.type = tarfile.LNKTYPE
        info.linkname = 'usr/include/bsd/stdlib.h'
        data = b''
    elif mode == 'fifo':
        info.type = tarfile.FIFOTYPE
        data = b''
    info.size = len(data)
    members.append((info, data))
with tarfile.open(work / 'cache' / name, 'w:gz', format=tarfile.USTAR_FORMAT) as archive:
    for info, data in members:
        archive.addfile(info, io.BytesIO(data) if info.isreg() else None)
PY
for mode in missing-header metadata script absolute traversal dot duplicate newline backslash \
    absolute-link parent-link dangling-link hardlink fifo; do
    reset_cache
    python3 "$work/mutate.py" "$work" "$mode"
    reseal
    expect_failure "$mode archive"
    [ ! -e "$work/escaped" ] || fail 'archive wrote outside staging'
done
reset_cache

# Reject symlink destinations and preserve their targets, even if dangling.
mv "$output" "$work/saved-root"
ln -s "$work/saved-root" "$output"
if invoke >"$work/failure.log" 2>&1; then fail 'accepted symlink root'; fi
rm "$output"
mv "$work/saved-root" "$output"
mv "$output.files" "$work/saved-files"
ln -s "$work/missing-manifest" "$output.files"
if invoke >"$work/failure.log" 2>&1; then fail 'accepted symlink manifest'; fi
rm "$output.files"
mv "$work/saved-files" "$output.files"
check_preserved

for rejected_output in "$src" "$cache" "$src/resources" "$src/system/rootfs" "$output/.."; do
    if PATH="$work/bin:$PATH" sh "$adapter" "$src" "$work/bin/deps" "$work/lock" \
        "$cache" "$work/bin/apk" "$rejected_output" >"$work/failure.log" 2>&1; then
        fail "accepted input overlap or aliased destination: $rejected_output"
    fi
done
printf 'protected lock\n' >"$work/protected.files"
cp "$work/protected.files" "$work/protected.original"
if PATH="$work/bin:$PATH" sh "$adapter" "$src" "$work/bin/deps" "$work/protected.files" \
    "$cache" "$work/bin/apk" "$work/protected" >"$work/failure.log" 2>&1; then
    fail 'accepted manifest/input overlap'
fi
grep -Fq 'manifest overlaps an input' "$work/failure.log"
cmp "$work/protected.original" "$work/protected.files"
check_preserved

# A deleted non-primary header must invalidate the presence signature, then be
# restored by the next adapter run with the same complete manifest.
signature=$work/presence.sig
printf 'signature\n' >"$signature"
touch -t 200001010000 "$signature"
cp -p "$signature" "$work/original.sig"
sh "$repo/tools/build/upstream-verify.sh" "$output" "$output.files" "$signature"
[ "$signature" -ot "$output/.complete" ] || fail 'intact inventory changed signature'
rm "$output/usr/include/bsd/nested/extra.h"
sh "$repo/tools/build/upstream-verify.sh" "$output" "$output.files" "$signature"
[ "$signature" -nt "$work/original.sig" ] || fail 'deleted header did not invalidate signature'
invoke
test -s "$output/usr/include/bsd/nested/extra.h"
cmp "$work/before.files" "$output.files"

# Also exercise invocation entirely through relative arguments.
(cd "$work" && PATH="$work/bin:$PATH" sh "$adapter" source bin/deps lock cache bin/apk relative-root)
test -s "$work/relative-root/lib/libbsd.so.0"
printf 'libbsd development: relocation, local-libmd linking, inventory recovery and failure safety passed\n'
