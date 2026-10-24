#!/bin/sh
# Production host tools must follow native commands and same-path tool changes.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd -P)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM

# Model a normal launcher + driver command. Both are stable paths, so changing
# either file must invalidate Kbuild's saved host commands.
cat > "$work/launcher" <<'LAUNCHER'
#!/bin/sh
exec "$@"
LAUNCHER
cat > "$work/driver" <<'DRIVER'
#!/bin/sh
exec cc "$@"
DRIVER
chmod +x "$work/launcher" "$work/driver"
hostcc="$work/launcher $work/driver"

make -C "$root" O="$work/out" HOSTCC="$hostcc" -j4 V=1 tools > "$work/first.log" 2>&1
object=$work/out/tools/host/common/buffer.o
before_object=$(stat -c '%y' "$object")
find "$work/out/host/bin" -maxdepth 1 -type f -perm /111 \
    -printf '%f %T@\n' | LC_ALL=C sort > "$work/links.before"
make -C "$root" O="$work/out" HOSTCC="$hostcc" -j4 V=1 tools > "$work/noop.log" 2>&1
[ "$before_object" = "$(stat -c '%y' "$object")" ] || {
    echo 'FAIL - no-op invocation changed a host object timestamp' >&2; exit 1;
}
find "$work/out/host/bin" -maxdepth 1 -type f -perm /111 \
    -printf '%f %T@\n' | LC_ALL=C sort > "$work/links.after"
cmp "$work/links.before" "$work/links.after"
if grep -Eq '^(HOSTCC|HOSTCXX|HOSTLD|HOSTAR|CC|LD|AR)[[:space:]]' "$work/noop.log" ||
        grep -Eq '^env RELIEFOS_HOST_COMPILER_ID=' "$work/noop.log"; then
    echo 'FAIL - no-op invocation emitted a compile or link command' >&2
    exit 1
fi

printf '# launcher replaced in-place\n' >> "$work/launcher"
make -C "$root" O="$work/out" HOSTCC="$hostcc" -j4 V=1 tools > "$work/launcher-replaced.log" 2>&1
before_driver=$(stat -c '%y' "$object")
[ "$before_object" != "$before_driver" ] || {
    echo 'FAIL - same-path HOSTCC launcher replacement left a stale object' >&2; exit 1;
}

printf '# driver replaced in-place\n' >> "$work/driver"
make -C "$root" O="$work/out" HOSTCC="$hostcc" -j4 V=1 tools > "$work/driver-replaced.log" 2>&1
[ "$before_driver" != "$(stat -c '%y' "$object")" ] || {
    echo 'FAIL - same-path wrapped HOSTCC driver replacement left a stale object' >&2; exit 1;
}

test -s "$work/out/tools/host/common/.buffer.o.cmd"
test -s "$work/out/host/bin/.reliefos-emit.cmd"
"$work/out/host/bin/reliefos-emit" --input "$root/configs/build-version" --output "$work/version"
cmp "$root/configs/build-version" "$work/version"
printf 'native host tools: no-op compile/link, launcher replacement and wrapped driver replacement passed\n'
