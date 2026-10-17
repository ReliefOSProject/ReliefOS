#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
tmp_root=${TMPDIR:-$root/out/tmp}
mkdir -p "$tmp_root"
work=$(mktemp -d "$tmp_root/reliefos-x11-stage.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
cache=$work/cache
package_root=$work/packages
stage=$work/out/upstream/x11-development
mkdir -p "$cache" "$package_root" "$(dirname "$stage")" "$work/bin"

cat >"$work/bin/deps" <<'DATA'
#!/bin/sh
set -eu
id= print=
while [ "$#" -gt 0 ]; do
    case $1 in
        --id) id=$2; shift ;;
        --print) print=$2; shift ;;
    esac
    shift
done
case $print in
    url) printf 'https://fixture.invalid/%s.apk\n' "$id" ;;
    sha256) sha256sum "$FIXTURE_CACHE/$id.apk" | cut -d' ' -f1 ;;
    *) exit 2 ;;
esac
DATA
cat >"$work/bin/apk" <<'DATA'
#!/bin/sh
set -eu
archive=
for argument do archive=$argument; done
id=${archive##*/}
id=${id%.apk}
if [ "${FAIL_VERIFY_ID:-}" = "$id" ]; then exit 1; fi
DATA
chmod +x "$work/bin/deps" "$work/bin/apk"

make_package() {
    package=$1
    shift
    directory=$package_root/$package
    mkdir -p "$directory"
    for path do
        mkdir -p "$directory/$(dirname "$path")"
        printf '%s\n' "$path" >"$directory/$path"
    done
    (cd "$directory" && tar -czf "$cache/$package.apk" usr)
}

make_package x11-dev-fltk \
    usr/include/FL/Fl.H usr/lib/libfltk.so usr/lib/libfltk.so.1.3
make_package x11-dev-libx11 \
    usr/include/X11/Xlib.h usr/include/X11/X.h usr/lib/libX11.so
make_package x11-dev-xorgproto usr/include/X11/extensions/Xproto.h
make_package alpine-libfltk usr/lib/libfltk.so.1.3
make_package alpine-libstdc++ usr/lib/libstdc++.so.6
make_package alpine-libgcc usr/lib/libgcc_s.so.1

mkdir -p "$stage/usr/include/Xm" "$stage/usr/lib"
printf 'old Motif header\n' >"$stage/usr/include/Xm/Xm.h"
printf 'old Motif runtime\n' >"$stage/usr/lib/libXm.so"
printf 'old completion marker\n' >"$stage/.complete"
before=$(cd "$stage" && find . -type f -exec sha256sum {} \; | LC_ALL=C sort)

if FIXTURE_CACHE=$cache FAIL_VERIFY_ID=alpine-libfltk \
    "$root/tools/build/x11-development.sh" "$root" "$work/bin/deps" \
    "$root/configs/dependencies.lock.json" "$cache" "$work/bin/apk" "$stage"; then
    printf 'expected package verification failure\n' >&2
    exit 1
fi
after=$(cd "$stage" && find . -type f -exec sha256sum {} \; | LC_ALL=C sort)
if [ "$before" != "$after" ]; then
    printf 'failed FLTK staging changed the previous development tree\n' >&2
    exit 1
fi

FIXTURE_CACHE=$cache "$root/tools/build/x11-development.sh" "$root" \
    "$work/bin/deps" "$root/configs/dependencies.lock.json" "$cache" \
    "$work/bin/apk" "$stage"
test -f "$stage/.complete"
test -f "$stage/usr/include/FL/Fl.H"
test -f "$stage/usr/include/X11/Xlib.h"
test -f "$stage/usr/lib/libfltk.so.1.3"
test -f "$stage/usr/lib/libstdc++.so.6"
test -f "$stage/usr/lib/libgcc_s.so.1"
test ! -e "$stage/usr/include/Xm/Xm.h"
test ! -e "$stage/usr/lib/libXm.so"
printf 'x11-development staging replacement tests passed\n'
