#!/bin/bash
# Run only in the disposable ReliefOS acceptance image, never on the host.
set -u
suite=/usr/lib/reliefos/tests/coreutils
probe=$suite/probe
gnu=/usr/bin
bb=/bin/busybox
work=/var/tmp/reliefos-coreutils-qa
data=$suite/data
export LC_ALL=C LANG=C PATH=/usr/bin:/usr/sbin:/bin:/sbin HOME=/root USER=root LOGNAME=root
unset POSIXLY_CORRECT
if [[ "$($bb uname -s)" != ReliefNT || "$($bb id -u)" != 0 ]]; then
    echo 'CU_ABORT private ReliefOS root guest required'; exit 1
fi
$bb mkdir -p "$work/listing" || exit 1
cd "$work" || exit 1
printf abc > abc
printf '' > empty
printf 'alpha\nbeta\ngamma\n' > lines
printf 'a\nb\n' > left
printf 'b\nc\n' > right
printf 'a 1\nb 3\n' > join-left
printf 'a 2\nb 4\n' > join-right
printf 'one:two:three\n' > fields
printf 'alpha beta gamma delta\n' > paragraph
printf '\ta\n' > tab
printf '        a\n' > spaces
printf 'b\na\nb\n' > unsorted
printf 'a\na\nb\n' > adjacent
printf 'a b\nb c\n' > graph
printf 'alpha beta\n' > keywords
printf 'a\n' > listing/a
printf 'b\n' > listing/b
export CU_TEST=value
failures=0 unsupported=0 covered=0 versions=0
input=$work/empty
last_status=0
invoke() {
    local want=$1; shift
    last_status=0
    "$probe" run 90000 "$input" "$work/out" "$work/err" -- "$@" || last_status=$?
    [[ "$last_status" == "$want" ]]
}
text() { local want=$1; shift; invoke 0 "$@" && [[ "$(<"$work/out")" == "$want" ]]; }
contains() { local want=$1; shift; invoke 0 "$@" && [[ "$(<"$work/out")" == *"$want"* ]]; }
same() { $bb cmp "$1" "$2"; }
functional() {
    local name=$1 value
    input=$work/empty
    case "$name" in
        '[') invoke 0 "$gnu/[" 3 -eq 3 ']' && invoke 1 "$gnu/[" 3 -eq 4 ']' ;;
        arch) text x86_64 "$gnu/arch" ;;
        coreutils) text dispatch "$gnu/coreutils" --coreutils-prog=printf '%s\n' dispatch ;;
        hostname) contains reliefos "$gnu/hostname" ;;
        kill) invoke 0 "$gnu/kill" -0 "$$" ;;
        uptime) contains up "$gnu/uptime" ;;
        chroot) text rooted "$gnu/chroot" / "$gnu/printf" '%s\n' rooted ;;
        df) contains Filesystem "$gnu/df" -P "$work" ;;
        hostid) invoke 0 "$gnu/hostid" && [[ "$(<"$work/out")" =~ ^[0-9a-fA-F]{8}$ ]] ;;
        nice) text nice-ok "$gnu/nice" -n 0 "$gnu/printf" '%s\n' nice-ok ;;
        pinky) contains root "$gnu/pinky" -l root ;;
        stdbuf) text abc "$gnu/stdbuf" -oL "$gnu/cat" "$work/abc" && [[ ! -s "$work/err" ]] ;;
        stty) input=/dev/tty1; invoke 0 "$gnu/stty" -g && [[ "$(<"$work/out")" == *:* ]] ;;
        timeout) invoke 124 "$gnu/timeout" 0.05 "$gnu/sleep" 1 ;;
        users|who) text '' "$gnu/$name" "$work/empty" ;;
        chcon) invoke 1 "$gnu/chcon" user_u:role_r:type_t:s0 "$work/abc" && [[ -s "$work/err" ]] ;;
        runcon) invoke 125 "$gnu/runcon" user_u:role_r:type_t:s0 "$gnu/true" && [[ -s "$work/err" ]] ;;
        b2sum|md5sum|sha1sum|sha224sum|sha256sum|sha384sum|sha512sum)
            IFS= read -r value < "$data/abc.$name"
            input=$work/abc
            invoke 0 "$gnu/$name" && [[ "$(<"$work/out")" == "$value  -" ]]
            ;;
        base32) input=$work/abc; text MFRGG=== "$gnu/base32" ;;
        base64) input=$work/abc; text YWJj "$gnu/base64" ;;
        basenc) input=$work/abc; text 616263 "$gnu/basenc" --base16 ;;
        basename) text file "$gnu/basename" /one/two/file.txt .txt ;;
        cat) text abc "$gnu/cat" "$work/abc" ;;
        chgrp) invoke 0 "$gnu/chgrp" 0 "$work/abc" && text 0 "$gnu/stat" -c %g "$work/abc" ;;
        chmod) invoke 0 "$gnu/chmod" 600 "$work/abc" && text 600 "$gnu/stat" -c %a "$work/abc" ;;
        chown) invoke 0 "$gnu/chown" 0:0 "$work/abc" && text 0:0 "$gnu/stat" -c %u:%g "$work/abc" ;;
        cksum) input=$work/abc; text '1219131554 3' "$gnu/cksum" ;;
        comm) text b "$gnu/comm" -12 "$work/left" "$work/right" ;;
        cp) invoke 0 "$gnu/cp" --reflink=never "$work/abc" "$work/copied" && same abc copied ;;
        csplit) invoke 0 "$gnu/csplit" -s -f "$work/piece" "$work/lines" 2 &&
            [[ "$(<piece00)" == alpha && "$(<piece01)" == $'beta\ngamma' ]] ;;
        cut) text two "$gnu/cut" -d : -f 2 "$work/fields" ;;
        date) text 1970-01-01T00:00:00Z "$gnu/date" -u -d @0 +%Y-%m-%dT%H:%M:%SZ ;;
        dd) invoke 0 "$gnu/dd" if="$work/abc" of="$work/dd-copy" bs=1 count=3 status=none && same abc dd-copy ;;
        dir|vdir|ls) text $'a\nb' "$gnu/$name" -1 "$work/listing" ;;
        dircolors) contains TERM "$gnu/dircolors" --print-database ;;
        dirname) text /one/two "$gnu/dirname" /one/two/file ;;
        du) invoke 0 "$gnu/du" -b "$work/abc" && [[ "$(<"$work/out")" == $'3\t'"$work/abc" ]] ;;
        echo) text 'hello world' "$gnu/echo" hello world ;;
        env) text clean "$gnu/env" -i CU_CLEAN=clean "$gnu/printenv" CU_CLEAN ;;
        expand) text '        a' "$gnu/expand" "$work/tab" ;;
        expr) text 42 "$gnu/expr" 7 '*' 6 ;;
        factor) text '84: 2 2 3 7' "$gnu/factor" 84 ;;
        false) invoke 1 "$gnu/false" ;;
        fmt) text $'alpha beta\ngamma delta' "$gnu/fmt" -w 12 "$work/paragraph" ;;
        fold) input=$work/abc; text $'ab\nc' "$gnu/fold" -b -w 2 ;;
        groups) contains root "$gnu/groups" root ;;
        head) text alpha "$gnu/head" -n 1 "$work/lines" ;;
        id) text 0 "$gnu/id" -u ;;
        install) invoke 0 "$gnu/install" -m 640 "$work/abc" "$work/installed" && same abc installed &&
            text 640 "$gnu/stat" -c %a "$work/installed" ;;
        join) text $'a 1 2\nb 3 4' "$gnu/join" "$work/join-left" "$work/join-right" ;;
        link) invoke 0 "$gnu/link" "$work/abc" "$work/hardlink" && same abc hardlink &&
            text 2 "$gnu/stat" -c %h "$work/abc" ;;
        ln) invoke 0 "$gnu/ln" -s abc "$work/symlink" && [[ -L symlink ]] ;;
        logname)
            # This OpenRC service has no login session or terminal on stdin.
            # GNU correctly rejects that context. A root-owned tty supplies
            # the real tty identity used by gnulib on systems without utmp.
            invoke 1 "$gnu/logname" && [[ "$(<"$work/err")" == *'no login name'* ]] || return 1
            input=/dev/tty1; text root "$gnu/logname"
            ;;
        mkdir) invoke 0 "$gnu/mkdir" -p "$work/new/child" && [[ -d new/child ]] ;;
        mkfifo) invoke 0 "$gnu/mkfifo" "$work/fifo" && [[ -p fifo ]] ;;
        mknod) invoke 0 "$gnu/mknod" "$work/node-fifo" p && [[ -p node-fifo ]] ;;
        mktemp) invoke 0 "$gnu/mktemp" -d "$work/temp.XXXXXX" && [[ -d "$(<"$work/out")" ]] ;;
        mv) printf moving > moving; invoke 0 "$gnu/mv" "$work/moving" "$work/moved" &&
            [[ ! -e moving && "$(<moved)" == moving ]] ;;
        nl) text $'1:alpha\n2:beta\n3:gamma' "$gnu/nl" -ba -w 1 -s : "$work/lines" ;;
        nproc) invoke 0 "$gnu/nproc" --all && [[ "$(<"$work/out")" =~ ^[1-9][0-9]*$ ]] ;;
        nohup) text nohup-ok "$gnu/nohup" "$gnu/printf" '%s\n' nohup-ok ;;
        numfmt) text 1024 "$gnu/numfmt" --from=iec 1K ;;
        od) text ' 61 62 63' "$gnu/od" -An -tx1 "$work/abc" ;;
        paste) text $'a,b\nb,c' "$gnu/paste" -d , "$work/left" "$work/right" ;;
        pathchk) invoke 0 "$gnu/pathchk" -p portable-name ;;
        pr) text $'alpha\nbeta\ngamma' "$gnu/pr" -t "$work/lines" ;;
        printenv) text value "$gnu/printenv" CU_TEST ;;
        printf) text 'value:0042' "$gnu/printf" '%s:%04d\n' value 42 ;;
        ptx) invoke 0 "$gnu/ptx" "$work/keywords" &&
            [[ "$(<"$work/out")" == *alpha* && "$(<"$work/out")" == *beta* ]] ;;
        pwd) text "$work" "$gnu/pwd" -P ;;
        readlink) text abc "$gnu/readlink" "$work/symlink" ;;
        realpath) text "$work/abc" "$gnu/realpath" "$work/symlink" ;;
        rm) printf remove > remove; invoke 0 "$gnu/rm" "$work/remove" && [[ ! -e remove ]] ;;
        rmdir) invoke 0 "$gnu/rmdir" "$work/new/child" && [[ ! -d new/child ]] ;;
        seq) text $'08\n09\n10' "$gnu/seq" -w 8 10 ;;
        shred) printf abc > shreddable
            invoke 0 "$gnu/shred" -n 1 -z -s 3 "$work/shreddable" &&
                text ' 00 00 00' "$gnu/od" -An -tx1 "$work/shreddable" ;;
        shuf) invoke 0 "$gnu/shuf" -i 1-5 && $bb cp "$work/out" "$work/shuffled" &&
            text $'1\n2\n3\n4\n5' "$gnu/sort" -n "$work/shuffled" ;;
        sleep) invoke 0 "$gnu/sleep" 0.02 ;;
        sort) text $'a\nb' "$gnu/sort" -u "$work/unsorted" ;;
        split) invoke 0 "$gnu/split" -l 1 "$work/lines" "$work/split." &&
            [[ "$(<split.aa)" == alpha && "$(<split.ab)" == beta && "$(<split.ac)" == gamma ]] ;;
        stat) text '3:regular file' "$gnu/stat" --printf=%s:%F "$work/abc" ;;
        sum) input=$work/abc; IFS= read -r value < "$data/abc.sum"; text "$value" "$gnu/sum" ;;
        sync) invoke 0 "$gnu/sync" -f "$work/abc" ;;
        tac) text $'gamma\nbeta\nalpha' "$gnu/tac" "$work/lines" ;;
        tail) text gamma "$gnu/tail" -n 1 "$work/lines" ;;
        tee) input=$work/abc; text abc "$gnu/tee" "$work/tee-copy" && same abc tee-copy ;;
        test) invoke 0 "$gnu/test" 3 -eq 3 && invoke 1 "$gnu/test" 3 -eq 4 ;;
        touch) invoke 0 "$gnu/touch" -d @1234567890 "$work/timestamp" &&
            text 1234567890 "$gnu/stat" -c %Y "$work/timestamp" ;;
        tr) input=$work/abc; text ABC "$gnu/tr" a-z A-Z ;;
        true) invoke 0 "$gnu/true" ;;
        truncate) invoke 0 "$gnu/truncate" -s 9 "$work/truncated" &&
            text 9 "$gnu/stat" -c %s "$work/truncated" ;;
        tsort) text $'a\nb\nc' "$gnu/tsort" "$work/graph" ;;
        tty) input=/dev/tty1; text /dev/tty1 "$gnu/tty" ;;
        uname) text ReliefNT "$gnu/uname" -s ;;
        unexpand) text $'\ta' "$gnu/unexpand" -a "$work/spaces" ;;
        uniq) text $'a\nb' "$gnu/uniq" "$work/adjacent" ;;
        unlink) invoke 0 "$gnu/unlink" "$work/hardlink" && [[ ! -e hardlink ]] ;;
        wc) input=$work/abc; text 3 "$gnu/wc" -c ;;
        whoami) text root "$gnu/whoami" ;;
        yes) text $'hello\nhello\nhello' /bin/bash -c '"$1/yes" hello | "$1/head" -n 3' qa "$gnu" ;;
        *) echo "CU_UNCOVERED $name"; return 1 ;;
    esac
}

echo "CU_START $(uname -r)"
while IFS= read -r name; do
    input=$work/empty
    if [[ "$name" == test ]]; then
        good=0; invoke 0 "$gnu/test" 3 -eq 3 && good=1
    else
        good=0
        version_status=0
        [[ "$name" != false ]] || version_status=1
        if invoke "$version_status" "$gnu/$name" --version &&
            { [[ "$(<"$work/out")" == *'(GNU coreutils) 9.12'* ]] ||
              [[ "$name" == dd && "$(<"$work/out")" == *'(coreutils) 9.12'* ]]; }; then good=1; fi
    fi
    if [[ "$good" == 1 && "$("$gnu/readlink" -f "$gnu/$name")" == "$gnu/coreutils" &&
          "$(type -P "$name")" == "$gnu/$name" ]]; then
        echo "CU_VERSION $name PASS"; versions=$((versions+1))
    else
        echo "CU_VERSION $name FAIL status=$last_status"; failures=$((failures+1))
        printf 'CU_STDOUT %q\n' "$(<"$work/out")"
        printf 'CU_STDERR %q\n' "$(<"$work/err")"
    fi
done < "$suite/programs.list"

# Query the real installed APK database, including paths transferred from older
# BusyBox packages. Do this once rather than reparsing the database 115 times.
owners=()
while IFS= read -r name; do owners+=("$gnu/$name"); done < "$suite/programs.list"
owners+=(/bin/cat /bin/ls /bin/cp /bin/rm /bin/false /usr/sbin/chroot)
input=$work/empty
if invoke 0 /sbin/apk info --who-owns "${owners[@]}"; then
    owner_count=0 owner_bad=0
    while IFS= read -r line; do
        [[ "$line" == *' is owned by reliefos-coreutils-'* ]] && owner_count=$((owner_count+1)) || owner_bad=1
    done < "$work/out"
    if [[ "$owner_count" == 115 && "$owner_bad" == 0 ]]; then echo 'CU_OWNERSHIP PASS paths=115'
    else echo "CU_OWNERSHIP FAIL paths=$owner_count"; failures=$((failures+1)); fi
else echo "CU_OWNERSHIP FAIL status=$last_status"; failures=$((failures+1)); fi
if ! "$gnu/cat" "$work/out"; then failures=$((failures+1)); fi

for path in /bin/cat /bin/ls /bin/cp /bin/rm /bin/false /usr/sbin/chroot; do
    if [[ "$("$gnu/readlink" -f "$path")" == "$gnu/coreutils" ]]; then echo "CU_ALIAS $path PASS"
    else echo "CU_ALIAS $path FAIL"; failures=$((failures+1)); fi
done

while IFS= read -r name; do
    if functional "$name"; then
        covered=$((covered+1))
        if [[ "$name" == chcon || "$name" == runcon ]]; then
            echo "CU_FUNC $name UNSUPPORTED expected-SELinux-error"; unsupported=$((unsupported+1))
        else echo "CU_FUNC $name PASS"; fi
    else
        echo "CU_FUNC $name FAIL status=$last_status"; failures=$((failures+1))
        printf 'CU_STDOUT %q\n' "$(<"$work/out")"
        printf 'CU_STDERR %q\n' "$(<"$work/err")"
    fi
done < "$suite/programs.list"

# Bare shell builtins remain shell builtins; all external command paths above
# are GNU. Explicit static BusyBox calls remain available for recovery scripts.
if [[ "$($bb cat "$work/abc")" == abc && "$($bb printf '%s' recovery)" == recovery ]]; then
    echo 'CU_BUSYBOX_RECOVERY PASS'
else echo 'CU_BUSYBOX_RECOVERY FAIL'; failures=$((failures+1)); fi

# Kernel-owned tests verify that this run has a real, trusted TSC clock. The
# SMP PIT fallback can undercount long uninterruptible disk commits; such data
# must not be published as wall-time throughput.
input=$work/empty
if invoke 0 "$suite/vdso-static" --require-hres; then
    echo 'CU_CLOCK PASS TSC-high-resolution'
else
    echo 'CU_CLOCK FAIL high-resolution-clock-required'
    "$gnu/cat" "$work/out" "$work/err"
    failures=$((failures+1))
    echo "CU_DONE versions=$versions functions=$covered unsupported=$unsupported failures=$failures"
    exit 1
fi

bench_bb=$suite/busybox-benchmark
[[ -x "$bench_bb" ]] || bench_bb=$bb
IFS= read -r bulk_hash < "$data/bulk.sha256"
read -r expect_lines expect_words expect_bytes < "$data/bulk.wc"
bytes=33554432
measure() {
    local label=$1 implementation=$2 count=$3 in=$4 out=$5; shift 5
    "$probe" bench "$label" "$implementation" 3 "$count" "$in" "$out" "$work/bench.err" -- "$@"
}
for implementation in gnu busybox; do
    for label in cat wc sha256sum tr sort cp; do
        if [[ "$implementation" == gnu ]]; then command=("$gnu/$label")
        else command=("$bench_bb" "$label"); fi
        ok=0
        case "$label" in
            cat) measure cat "$implementation" "$bytes" "$data/bulk" /dev/null "${command[@]}" &&
                { input=$data/bulk; text "$bulk_hash  -" /bin/bash -o pipefail -c \
                    '"$@" | /usr/bin/sha256sum' qa "${command[@]}"; } && ok=1 ;;
            wc) measure wc "$implementation" "$bytes" "$data/bulk" "$work/bench.out" "${command[@]}" -l -w -c &&
                read -r got_lines got_words got_bytes < "$work/bench.out" &&
                [[ "$got_lines" == "$expect_lines" && "$got_words" == "$expect_words" &&
                   "$got_bytes" == "$expect_bytes" ]] && ok=1 ;;
            sha256sum) measure sha256sum "$implementation" "$bytes" "$work/empty" "$work/bench.out" "${command[@]}" "$data/bulk" &&
                [[ "$(<"$work/bench.out")" == "$bulk_hash  $data/bulk" ]] && ok=1 ;;
            tr) measure tr "$implementation" "$bytes" "$data/bulk" /dev/null "${command[@]}" a-z A-Z &&
                IFS= read -r upper_hash < "$data/upper.sha256" &&
                { input=$data/bulk; text "$upper_hash  -" /bin/bash -o pipefail -c \
                    '"$@" a-z A-Z | /usr/bin/sha256sum' qa "${command[@]}"; } && ok=1 ;;
            sort) measure sort "$implementation" 2359296 "$data/numbers" /dev/null "${command[@]}" &&
                IFS= read -r sorted_hash < "$data/sorted.sha256" &&
                { input=$data/numbers; text "$sorted_hash  -" /bin/bash -o pipefail -c \
                    '"$@" | /usr/bin/sha256sum' qa "${command[@]}"; } && ok=1 ;;
            cp) measure cp "$implementation" 1048576 "$work/empty" /dev/null "${command[@]}" "$data/copy-source" "$work/bulk-copy" &&
                same "$data/copy-source" "$work/bulk-copy" && ok=1
                $bb rm -f "$work/bulk-copy"
                ;;
        esac
        if [[ "$ok" == 1 ]]; then echo "CU_BENCH_CHECK $label $implementation PASS"
        else echo "CU_BENCH_CHECK $label $implementation FAIL"; failures=$((failures+1)); fi
    done
done
echo "CU_DONE versions=$versions functions=$covered unsupported=$unsupported failures=$failures"
[[ "$versions" == 109 && "$covered" == 109 && "$failures" == 0 ]]
