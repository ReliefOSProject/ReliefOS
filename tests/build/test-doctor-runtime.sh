#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd -P)
w=$(mktemp -d)
trap 'rm -rf "$w"' EXIT
real_cc=${TEST_CLANG:-/usr/bin/clang}
cat > "$w/clang" <<'SH'
#!/bin/sh
for arg do
 case "$arg" in *print-libgcc-file-name*) printf '%s/missing-builtins.a\n' "$TEST_WORK"; exit 0;; esac
done
exec "$TEST_REAL_CC" "$@"
SH
chmod +x "$w/clang"
export TEST_WORK="$w" TEST_REAL_CC="$real_cc"
HOSTCC="env RELIEFOS_DOCTOR_WRAPPER=1 ${HOSTCC:-cc}" \
 TARGET_CC="$w/clang" TARGET_LD=ld.lld TARGET_AR=llvm-ar TARGET_OBJCOPY=llvm-objcopy \
 TARGET_STRIP=llvm-strip TARGET_TRIPLE_KERNEL=x86_64-unknown-none \
 TARGET_TRIPLE_USER=x86_64-linux-musl SRC="$repo" sh "$repo/scripts/doctor.sh" > "$w/log" 2>&1 && status=0 || status=$?
if [ "$status" -eq 0 ] || ! grep -q 'MISSING.*compiler-rt builtins' "$w/log"; then
 echo 'FAIL: doctor did not diagnose absent target builtins archive'; cat "$w/log"; exit 1
fi
echo 'ok: doctor diagnoses missing compiler-rt archive despite valid headers/compiler'
if grep -q 'MISSING.*HOSTCC=' "$w/log" || ! grep -q 'OK .*host link verified' "$w/log"; then
 echo 'FAIL: doctor did not execute the wrapped host compiler'; cat "$w/log"; exit 1
fi
echo 'ok: doctor accepts and executes a wrapped host compiler'
grep -q 'OK .*flock' "$w/log" || { echo 'FAIL: doctor did not check flock'; exit 1; }
echo 'ok: doctor checks flock'
