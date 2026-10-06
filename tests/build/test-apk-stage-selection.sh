#!/bin/sh
# Contract test: apk-stage.sh installs only the upstream APKs of the selected
# desktop backend, and filtered archives leave no trace in any staging step.
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd -P)
w=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-apk-stage-selection.XXXXXX")
trap 'rm -rf "$w"' EXIT HUP INT TERM

hostcc=${HOSTCC:-cc}
"$hostcc" -std=c11 -Wall -Wextra -Werror -Wpedantic -I"$root" \
    "$root/tools/host/manifest/reliefos-layout.c" -o "$w/layout"

mkdir -p "$w/bin" "$w/upstream/packages" "$w/raw/etc/reliefos" \
    "$w/raw/usr/lib/xorg/modules" "$w/raw/usr/bin"

# The query tool is authoritative for selection: xorg-fonts is a base package
# despite its name, so name matching can never pass these assertions.
cat > "$w/bin/fake-deps" <<'DEPS'
#!/bin/sh
set -eu
lock= id= field=
while [ "$#" -gt 0 ]; do
    case $1 in
        --lock) lock=$2; shift 2 ;;
        --id) id=$2; shift 2 ;;
        --print) field=$2; shift 2 ;;
        *) echo "fake-deps: unexpected argument: $1" >&2; exit 1 ;;
    esac
done
printf '%s\t%s\t%s\n' "$lock" "$id" "$field" >> "$FAKE_DEPS_LOG"
[ "$field" = feature ] || { echo "fake-deps: unsupported field: $field" >&2; exit 1; }
case $id in
    alpine-openrc|alpine-xorg-fonts) printf 'base\n' ;;
    alpine-xorg-server|alpine-xterm) printf 'xorg\n' ;;
    alpine-broken-feature) printf 'weird\n' ;;
    *) echo "no dependency named \"$id\"" >&2; exit 1 ;;
esac
DEPS

cat > "$w/bin/fake-own" <<'OWN'
#!/bin/sh
set -eu
root= output= elf=
while [ "$#" -gt 0 ]; do
    case $1 in
        --root) root=$2; shift 2 ;;
        --output) output=$2; shift 2 ;;
        --elf-list) elf=$2; shift 2 ;;
        *) shift ;;
    esac
done
(cd "$root" && find . -mindepth 1 \( -type f -o -type l \) -printf '%P\n' | LC_ALL=C sort) > "$FAKE_OWN_LOG"
{
    while IFS= read -r path; do
        printf 'reliefos-base\tfile\t0644\t%s\t-\n' "$path"
    done < "$FAKE_OWN_LOG"
} > "$output"
: > "$elf"
OWN

cat > "$w/bin/fake-apk" <<'APK'
#!/bin/sh
set -eu
printf '%s\n' "$*" >> "$FAKE_APK_LOG"
command= files= output= root= repository= requests=
while [ "$#" -gt 0 ]; do
    case $1 in
        mkpkg|mkndx) command=$1; shift ;;
        add) command=add; shift; requests=$*; set -- ;;
        --files) files=$2; shift 2 ;;
        --output) output=$2; shift 2 ;;
        --root) root=$2; shift 2 ;;
        --repository) repository=$2; shift 2 ;;
        *) shift ;;
    esac
done
case $command in
    mkpkg)
        (cd "$files" && tar --ignore-zeros -czf "$output" .)
        ;;
    mkndx)
        : > "$output"
        ;;
    add)
        [ -n "$root" ] && [ -n "$repository" ] || { echo 'fake-apk: add needs --root and --repository' >&2; exit 1; }
        mkdir -p "$root"
        for request in $requests; do
            name=${request%%=*}
            found=0
            for archive in "$(dirname "$repository")"/$name-*.apk; do
                [ -f "$archive" ] || continue
                found=1
                tar --ignore-zeros --no-same-owner --exclude='.PKGINFO' -xzf "$archive" -C "$root"
            done
            [ "$found" = 1 ] || { echo "fake-apk: no archive for request: $request" >&2; exit 1; }
        done
        ;;
    *)
        echo "fake-apk: unknown command" >&2
        exit 1
        ;;
esac
APK
chmod 755 "$w/bin/fake-deps" "$w/bin/fake-own" "$w/bin/fake-apk"

build_archive() {
    destination=$1 name=$2 version=$3
    shift 3
    staging=$w/pack.$name
    rm -rf "$staging"
    mkdir -p "$staging"
    printf 'pkgname = %s\npkgver = %s\n' "$name" "$version" > "$staging/.PKGINFO"
    for member in "$@"; do
        mkdir -p "$staging/$(dirname "$member")"
        printf 'fixture payload for %s\n' "$member" > "$staging/$member"
    done
    (cd "$staging" && tar --ignore-zeros -czf "$destination/$name-$version.apk" .PKGINFO "$@")
}

build_archive "$w/upstream/packages" openrc 0.1 etc/fixture-openrc.conf etc/openrc-extra.conf
build_archive "$w/upstream/packages" xorg-fonts 0.1 usr/share/fonts/xorg-fonts.fixture
build_archive "$w/upstream/packages" xorg-server 0.1 usr/bin/xorg-server usr/lib/xorg/modules/raw-overlay-probe
build_archive "$w/upstream/packages" xterm 0.1 usr/bin/xterm

printf 'raw openrc overlay\n' > "$w/raw/etc/fixture-openrc.conf"
printf 'raw probe\n' > "$w/raw/usr/lib/xorg/modules/raw-overlay-probe"
printf 'raw only\n' > "$w/raw/usr/bin/keep-me.txt"
printf '{"fixture":true}\n' > "$w/policy.json"
printf '{"fixture":true}\n' > "$w/lock.json"

stage() {
    run=$1
    shift
    mkdir -p "$w/$run"
    export FAKE_DEPS_LOG=$w/$run/deps.log FAKE_APK_LOG=$w/$run/apk.log FAKE_OWN_LOG=$w/$run/own.log
    APK_LAYOUT_TOOL=$w/layout \
        sh "$root/tools/build/apk-stage.sh" "$root" "$w/raw" "$w/$run/managed" \
        "$w/$run" "$w/bin/fake-apk" "$w/upstream" "$w/bin/fake-deps" \
        "$w/lock.json" "$w/policy.json" "$w/bin/fake-own" "$w/$run/key.pem" \
        "1.0-r0" > "$w/$run/stdout" 2> "$w/$run/stderr"
}

expect_present() { [ -e "$1" ] || { echo "FAIL - missing: $1" >&2; exit 1; }; }
expect_absent() { [ ! -e "$1" ] || { echo "FAIL - unexpected: $1" >&2; exit 1; }; }
expect_grep() { grep -q "$2" "$1" || { echo "FAIL - $1 lacks: $2" >&2; exit 1; }; }
expect_no_grep() { ! grep -q "$2" "$1" || { echo "FAIL - $1 still has: $2" >&2; exit 1; }; }
expect_fail() {
    label=$1
    shift
    if "$@" > /dev/null 2>&1; then
        echo "FAIL - $label was accepted" >&2
        exit 1
    fi
}

set_marker() { printf '%s' "$1" > "$w/raw/etc/reliefos/desktop-backend"; }
newline='
'

# --- reliefos backend: only base feature archives take part in staging ---
set_marker "reliefos
"
stage reliefos
s=$w/reliefos
expect_present "$s/repository/openrc-0.1.apk"
expect_present "$s/repository/xorg-fonts-0.1.apk"
expect_absent "$s/repository/xorg-server-0.1.apk"
expect_absent "$s/repository/xterm-0.1.apk"
expect_present "$s/managed/usr/share/reliefos/apk/repository/openrc-0.1.apk"
expect_absent "$s/managed/usr/share/reliefos/apk/repository/xorg-server-0.1.apk"
expect_absent "$s/managed/usr/share/reliefos/apk/repository/xterm-0.1.apk"
expect_present "$s/managed/usr/bin/keep-me.txt"
expect_present "$s/managed/usr/lib/xorg/modules/raw-overlay-probe"
expect_present "$s/managed/etc/fixture-openrc.conf"
expect_present "$s/managed/etc/openrc-extra.conf"
expect_present "$s/managed/usr/share/fonts/xorg-fonts.fixture"
expect_absent "$s/managed/usr/bin/xorg-server"
expect_absent "$s/managed/usr/bin/xterm"
expect_grep "$s/own.log" 'usr/lib/xorg/modules/raw-overlay-probe'
expect_grep "$s/own.log" 'usr/bin/keep-me.txt'
expect_no_grep "$s/own.log" 'etc/fixture-openrc.conf'
expect_no_grep "$s/own.log" 'usr/bin/xorg-server'
expect_no_grep "$s/own.log" 'usr/bin/xterm'
expect_no_grep "$s/own.log" 'etc/openrc-extra.conf'
expect_no_grep "$s/apk.log" 'xorg-server'
expect_no_grep "$s/apk.log" 'xterm'
expect_grep "$s/apk.log" 'add reliefos-base openrc=0.1 xorg-fonts=0.1'
for id in alpine-openrc alpine-xorg-fonts alpine-xorg-server alpine-xterm; do
    expect_grep "$s/deps.log" "$id"
done
expect_grep "$s/deps.log" "$w/lock.json"

# --- xorg backend: base and xorg archives are all installed ---
set_marker "xorg
"
stage xorg
s=$w/xorg
for archive in openrc-0.1.apk xorg-fonts-0.1.apk xorg-server-0.1.apk xterm-0.1.apk; do
    expect_present "$s/repository/$archive"
    expect_present "$s/managed/usr/share/reliefos/apk/repository/$archive"
done
expect_present "$s/managed/usr/bin/xorg-server"
expect_present "$s/managed/usr/bin/xterm"
expect_present "$s/managed/etc/openrc-extra.conf"
expect_present "$s/managed/usr/share/fonts/xorg-fonts.fixture"
expect_present "$s/managed/usr/bin/keep-me.txt"
expect_grep "$s/own.log" 'usr/bin/keep-me.txt'
expect_no_grep "$s/own.log" 'usr/lib/xorg/modules/raw-overlay-probe'
expect_no_grep "$s/own.log" 'etc/fixture-openrc.conf'
expect_no_grep "$s/own.log" 'usr/bin/xorg-server'
expect_grep "$s/apk.log" 'add reliefos-base openrc=0.1 xorg-fonts=0.1 xorg-server=0.1 xterm=0.1'

# --- marker failures stop staging before any scan or transaction ---
for marker in '' 'bogus' 'reliefos extra' "reliefos${newline}xorg${newline}" 'xorg '; do
    set_marker "$marker"
    rm -rf "$w/marker-fail"
    mkdir -p "$w/marker-fail"
    export FAKE_DEPS_LOG=$w/marker-fail/deps.log FAKE_APK_LOG=$w/marker-fail/apk.log FAKE_OWN_LOG=$w/marker-fail/own.log
    expect_fail 'invalid marker' env APK_LAYOUT_TOOL=$w/layout \
        sh "$root/tools/build/apk-stage.sh" "$root" "$w/raw" "$w/marker-fail/managed" \
        "$w/marker-fail" "$w/bin/fake-apk" "$w/upstream" "$w/bin/fake-deps" \
        "$w/lock.json" "$w/policy.json" "$w/bin/fake-own" "$w/marker-fail/key.pem" "1.0-r0"
    expect_absent "$w/marker-fail/managed"
    expect_absent "$w/marker-fail/own.log"
    expect_absent "$w/marker-fail/apk.log"
    expect_absent "$w/marker-fail/repository"
done

rm -f "$w/raw/etc/reliefos/desktop-backend"
mkdir -p "$w/missing-marker"
export FAKE_DEPS_LOG=$w/missing-marker/deps.log FAKE_APK_LOG=$w/missing-marker/apk.log FAKE_OWN_LOG=$w/missing-marker/own.log
expect_fail 'missing marker' env APK_LAYOUT_TOOL=$w/layout \
    sh "$root/tools/build/apk-stage.sh" "$root" "$w/raw" "$w/missing-marker/managed" \
    "$w/missing-marker" "$w/bin/fake-apk" "$w/upstream" "$w/bin/fake-deps" \
    "$w/lock.json" "$w/policy.json" "$w/bin/fake-own" "$w/missing-marker/key.pem" "1.0-r0"
expect_absent "$w/missing-marker/managed"
expect_absent "$w/missing-marker/own.log"

# --- query failures name the package and the lock path ---
set_marker "reliefos
"
mkdir -p "$w/ghost-upstream/packages" "$w/broken-upstream/packages"
build_archive "$w/ghost-upstream/packages" ghost 0.1 usr/bin/ghost
build_archive "$w/broken-upstream/packages" broken-feature 0.1 usr/bin/broken-feature
for run in ghost broken-feature; do
    upstream=$w/$run-upstream
    rm -rf "$w/$run"
    mkdir -p "$w/$run"
    export FAKE_DEPS_LOG=$w/$run/deps.log FAKE_APK_LOG=$w/$run/apk.log FAKE_OWN_LOG=$w/$run/own.log
    if APK_LAYOUT_TOOL=$w/layout \
        sh "$root/tools/build/apk-stage.sh" "$root" "$w/raw" "$w/$run/managed" \
        "$w/$run" "$w/bin/fake-apk" "$upstream" "$w/bin/fake-deps" \
        "$w/lock.json" "$w/policy.json" "$w/bin/fake-own" "$w/$run/key.pem" "1.0-r0" \
        >"$w/$run/stdout" 2>"$w/$run/stderr"; then
        echo "FAIL - $run archive passed selection" >&2
        exit 1
    fi
    expect_absent "$w/$run/managed"
    expect_absent "$w/$run/repository"
    expect_absent "$w/$run/own.log"
done
expect_grep "$w/ghost/stderr" 'ghost'
expect_grep "$w/ghost/stderr" "$w/lock.json"
expect_grep "$w/broken-feature/stderr" 'broken-feature'

printf '%s\n' 'apk stage selection: base and xorg features filter every staging step'

