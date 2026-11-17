#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
src=$work/source build=$work/build dest=$work/destination
mkdir -p "$build/lib" "$build/modules" "$build/bin"
for lib in pam pam_misc pamc; do
    printf 'library fixture\n' > "$build/lib/lib$lib.so.0.1"
    chmod 0700 "$build/lib/lib$lib.so.0.1"
    ln -s "lib$lib.so.0.1" "$build/lib/lib$lib.so.0"
    ln -s "lib$lib.so.0" "$build/lib/lib$lib.so"
done
printf 'module fixture\n' > "$build/modules/pam_fixture.so"
chmod 0700 "$build/modules/pam_fixture.so"
for name in faillock mkhomedir_helper pwhistory_helper pam_timestamp_check unix_chkpwd upperLOWER; do
    printf 'helper fixture\n' > "$build/bin/$name"
done
mkdir -p "$src/libpam/include/security" "$src/libpamc/include/security" \
    "$src/libpam_misc/include/security" "$src/modules/pam_filter"
for header in _pam_compat _pam_macros _pam_types pam_appl pam_ext pam_modules pam_modutil; do
    : > "$src/libpam/include/security/$header.h"
done
: > "$src/libpamc/include/security/pam_client.h"
: > "$src/libpam_misc/include/security/pam_misc.h"
: > "$src/modules/pam_filter/pam_filter.h"
for pair in access:access env:pam_env group:group faillock:faillock limits:limits namespace:namespace pwhistory:pwhistory time:time; do
    module=${pair%%:*} conf=${pair#*:}
    mkdir -p "$src/modules/pam_$module"
    : > "$src/modules/pam_$module/$conf.conf"
done
: > "$src/modules/pam_env/environment"
for name in namespace.init pam_namespace_helper.in pam_namespace.service.in; do
    : > "$src/modules/pam_namespace/$name"
done
(umask 077; sh "$repo/tools/build/pam/install.sh" "$src" "$build" "$dest")
for lib in pam pam_misc pamc; do
    test "$(stat -c %a "$dest/lib/lib$lib.so.0.1")" = 755
    test "$(readlink "$dest/lib/lib$lib.so.0")" = "lib$lib.so.0.1"
done
test "$(stat -c %a "$dest/lib/security/pam_fixture.so")" = 755
test "$(stat -c %a "$dest/usr/include/security/pam_appl.h")" = 644
test "$(stat -c %a "$dest/lib")" = 755
test "$(stat -c %a "$dest/lib/security")" = 755
printf 'PAM install: library/module modes and symlinks survive restrictive host umask\n'
