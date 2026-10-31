#!/bin/sh
# Reject an incomplete or incorrectly routed GNU multi-call installation.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
mkdir -p "$src/out/tmp"
work=$(TMPDIR="$src/out/tmp" mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/root/usr/bin" "$work/root/usr/libexec/coreutils"
printf '%s\n' cat coreutils false stdbuf > "$work/programs"
printf '%s\n' cat coreutils false libstdbuf.so stdbuf > "$work/upstream"
printf '#!/bin/sh\nexit 0\n' > "$work/root/usr/bin/coreutils"
chmod 755 "$work/root/usr/bin/coreutils"
for name in cat false stdbuf; do ln -s coreutils "$work/root/usr/bin/$name"; done
printf 'target preload library\n' > "$work/root/usr/libexec/coreutils/libstdbuf.so"
check() { sh "$src/tools/build/coreutils-check.sh" "$work/root" "$work/programs" "$work/upstream"; }
if ! check > "$work/report" 2>&1; then
    cat "$work/report" >&2
    echo 'FAIL - complete GNU installation rejected' >&2; exit 1
fi
rm "$work/root/usr/bin/cat"
if check > "$work/report" 2>&1; then echo 'FAIL - missing GNU command accepted' >&2; exit 1; fi
ln -s busybox "$work/root/usr/bin/cat"
if check > "$work/report" 2>&1; then echo 'FAIL - BusyBox command target accepted' >&2; exit 1; fi
rm "$work/root/usr/bin/cat"; ln -s coreutils "$work/root/usr/bin/cat"
chmod 644 "$work/root/usr/bin/coreutils"
if check > "$work/report" 2>&1; then echo 'FAIL - non-executable GNU dispatcher accepted' >&2; exit 1; fi
chmod 755 "$work/root/usr/bin/coreutils"
rm "$work/root/usr/libexec/coreutils/libstdbuf.so"
if check > "$work/report" 2>&1; then echo 'FAIL - missing stdbuf helper accepted' >&2; exit 1; fi
printf 'target preload library\n' > "$work/root/usr/libexec/coreutils/libstdbuf.so"
printf '%s\n' cat coreutils false stdbuf tail libstdbuf.so > "$work/upstream"
if check > "$work/report" 2>&1; then echo 'FAIL - omitted upstream command accepted' >&2; exit 1; fi
printf '%s\n' cat coreutils false libstdbuf.so stdbuf > "$work/upstream"
printf '%s\n' cat coreutils false stdbuf cat > "$work/programs"
if check > "$work/report" 2>&1; then echo 'FAIL - duplicate GNU command accepted' >&2; exit 1; fi
printf '%s\n' cat coreutils false stdbuf '../escape' > "$work/programs"
if check > "$work/report" 2>&1; then echo 'FAIL - unsafe GNU command accepted' >&2; exit 1; fi
printf '%s\n' 'GNU coreutils complete staged installation checks: PASS'
