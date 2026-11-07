#!/bin/sh
# Fixed ReliefOS feature policy, derived from Linux-PAM 1.7.3 meson.build defaults.
# Capability checks compile/link only; cross executables are never run.
set -eu
if [ "${1:-}" = --help ]; then
    echo 'usage: CC=... CPPFLAGS=... CFLAGS=... LDFLAGS=... configure.sh OUTPUT'
    exit 0
fi
[ "$#" = 1 ] || exit 2
output=$1
mkdir -p "$(dirname "$output")"
tmp=$(mktemp -d "$(dirname "$output")/.configure.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
: "${CC:?}" "${CPPFLAGS:=}" "${CFLAGS:=}" "${LDFLAGS:=}"
cat > "$tmp/config.h" <<'HEADER'
/* Linux-PAM 1.7.3 ReliefOS policy; capabilities below are cross-link probes. */
#pragma once
#define _GNU_SOURCE 1
#define PACKAGE "Linux-PAM"
#define PAM_VERSION "1.7.3"
#define UNUSED __attribute__((__unused__))
#define PAM_NO_HEADER_FUNCTIONS 1
#define SYSCONFDIR "/etc"
#define SCONFIG_DIR "/etc/security"
#define SCONFIGDIR /etc/security
#define sbindir /sbin
#define LTDIR ""
#define _PAM_ISA "../../lib/security"
#define DEFAULT_USERGROUPS_SETTING 0
#define PAM_USERTYPE_UIDMIN 1000
#define PAM_USERTYPE_OVERFLOW_UID 65534
#define PAM_MISC_CONV_BUFSIZE 4096
#define PAM_PATH_RANDOMDEV "/dev/urandom"
#define PAM_PATH_MAILDIR _PATH_MAILDIR
#define USE_LCKPWDF 1
HEADER
probe() {
    name=$1
    libs=$2
    # Flags are the conventional whitespace-separated compiler flag variables.
    if $CC $CPPFLAGS $CFLAGS -D_GNU_SOURCE -Werror=implicit-function-declaration \
        "$tmp/test.c" $LDFLAGS $libs -o "$tmp/test" >>"$tmp/probes.log" 2>&1; then
        printf '#define %s 1\n' "$name" >> "$tmp/config.h"
        return 0
    fi
    return 1
}
for header in crypt.h paths.h sys/random.h; do
    printf '#include <%s>\nint main(void) { return 0; }\n' "$header" > "$tmp/test.c"
    macro=$(printf '%s' "$header" | tr 'a-z/.' 'A-Z__')
    probe "HAVE_$macro" '' || :
done
while read -r func header libs; do
    printf '#include <%s>\nint main(void) { __typeof__(&%s) volatile p = &%s; return p == 0; }\n' "$header" "$func" "$func" > "$tmp/test.c"
    macro=$(printf '%s' "$func" | tr 'a-z' 'A-Z')
    probe "HAVE_$macro" "$libs" || :
done <<'FUNCTIONS'
close_range unistd.h
explicit_bzero string.h
getdomainname unistd.h
getgrgid_r grp.h
getgrnam_r grp.h
getgrouplist grp.h
getmntent_r mntent.h
getpwnam pwd.h
getpwnam_r pwd.h
getpwuid_r pwd.h
getrandom sys/random.h
getspnam_r shadow.h
getutent_r utmp.h
innetgr netdb.h
memset_explicit string.h
quotactl sys/quota.h
ruserok netdb.h
ruserok_af netdb.h
unshare sched.h
lckpwdf shadow.h
crypt_r crypt.h -lcrypt
crypt_rn crypt.h -lcrypt
bindtextdomain libintl.h
dngettext libintl.h
FUNCTIONS
# The locked musl profile includes these modules; fail rather than silently drop them.
for required in HAVE_CRYPT_R HAVE_CRYPT_RN HAVE_UNSHARE HAVE_QUOTACTL; do
    grep -q "^#define $required 1$" "$tmp/config.h" || {
        cat "$tmp/probes.log" >&2
        echo "PAM: required cross capability missing: $required" >&2
        exit 1
    }
done
printf '#include <sys/syscall.h>\nint main(void) { return __NR_keyctl == 0; }\n' > "$tmp/test.c"
probe RELIEFOS_PAM_KEYINIT '' || { cat "$tmp/probes.log" >&2; exit 1; }
if grep -q '^#define HAVE_DNGETTEXT 1$' "$tmp/config.h"; then
    printf '#define ENABLE_NLS 1\n#define LOCALEDIR "/usr/share/locale"\n' >> "$tmp/config.h"
fi
mv "$tmp/config.h" "$output"
# Probe diagnostics are available on failure; no target binary escapes this directory.
