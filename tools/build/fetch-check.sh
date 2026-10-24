#!/bin/sh
# Probe the download URLs recorded in configs/dependencies.lock.json and name
# the ones that no longer answer.
#
# fetch.sh is the stage that moves bytes; this script only asks each locked URL
# whether it still serves, so a 404 or a vanished mirror surfaces before a
# build ever has to stop for missing cache bytes.  The URL list comes from the
# same reliefos-deps --fetch-list contract fetch.sh consumes, so a check covers
# exactly the packages `make fetch` would download and nothing else.  Nothing
# is downloaded here: probes are HEAD requests, confirmed by a one-byte ranged
# GET only when a mirror refuses HEAD.
set -eu

. "$(CDPATH= cd -- "$(dirname "$0")/../../scripts" && pwd)/logging.sh"

die() {
    printf 'fetch-check: %s\n' "$*" >&2
    exit 1
}

deps=""
lock=""
selected=""

while [ "$#" -gt 0 ]; do
    case "$1" in
        --deps) deps=$2; shift 2 ;;
        --lock) lock=$2; shift 2 ;;
        --only) selected=$2; shift 2 ;;
        --help|-h)
            printf '%s\n' \
                'usage: fetch-check.sh --deps PATH --lock PATH [--only id[,id]]' \
                '' \
                'Probe every URL fetch.sh would download and fail when one answers' \
                'with an HTTP error or not at all.  Nothing is downloaded.' >&2
            exit 2 ;;
        *) printf 'fetch-check: unknown argument: %s\n' "$1" >&2; exit 2 ;;
    esac
done

for required in deps lock; do
    eval "value=\${$required}"
    [ -n "$value" ] || { printf 'fetch-check: --%s is required\n' "$required" >&2; exit 2; }
done
[ -x "$deps" ] || die "$deps is not executable; run make tools first"
command -v curl >/dev/null 2>&1 || die "curl is required to probe the locked URLs"

wanted=$("$deps" --lock "$lock" --fetch-list)
[ -n "$wanted" ] || die "the lock file lists nothing to download"

# Probe one locked URL and set probe_result.  HEAD is the cheap primary; a
# mirror that refuses HEAD (405/403) can still serve the file, so anything
# outside 2xx/3xx is confirmed with a one-byte ranged GET before the URL is
# called broken.  probe_code 000 means curl failed below HTTP; the result then
# carries curl's exit code (6 name resolution, 7 connection, 28 timeout,
# 35 TLS, 60 certificate) instead of a status.
probe() {
    probe_url=$1
    probe_code=000
    probe_status=0
    probe_code=$(curl --silent --location --proto '=https' --tlsv1.2 --head \
        --output /dev/null --write-out '%{http_code}' \
        --connect-timeout 10 --max-time 20 --retry 1 --retry-delay 1 \
        "$probe_url") || probe_status=$?
    [ -n "$probe_code" ] || probe_code=000
    case "$probe_code" in
        [23]??) probe_result="HTTP $probe_code"; return 0 ;;
    esac
    probe_attempt=1
    while :; do
        probe_status=0
        probe_code=$(curl --silent --location --proto '=https' --tlsv1.2 \
            --range 0-0 --output /dev/null --write-out '%{http_code}' \
            --connect-timeout 10 --max-time 20 --retry 1 --retry-delay 1 \
            "$probe_url") || probe_status=$?
        [ -n "$probe_code" ] || probe_code=000
        case "$probe_code" in
            [23]??) probe_result="HTTP $probe_code"; return 0 ;;
            000)
                # A transport-level failure gets one delayed second look:
                # handshake bursts are not a dead mirror, and a check that
                # cries wolf on them stops being read.
                if [ "$probe_attempt" -lt 2 ]; then
                    probe_attempt=2
                    sleep 2
                    continue
                fi
                probe_result="curl exit $probe_status"
                return 1 ;;
            *) probe_result="HTTP $probe_code"; return 1 ;;
        esac
    done
}

TAB=$(printf '\t')
checked=0
failed=""
while IFS=$TAB read -r id digest url name; do
    [ -n "$id" ] || continue
    if [ -n "$selected" ]; then
        case ",$selected," in
            *",$id,"*) ;;
            *) continue ;;
        esac
    fi
    checked=$((checked + 1))
    if probe "$url"; then
        reliefos_log OK "$id ($probe_result)"
    else
        reliefos_log FAIL "$id ($probe_result) $url"
        failed="$failed $id"
    fi
done <<EOF
$wanted
EOF

[ "$checked" -gt 0 ] || die "nothing selected to check"
if [ -n "$failed" ]; then
    printf 'fetch-check: these locked URLs do not answer:%s\n' "$failed" >&2
    printf '       fix the mirror or the lock entry, then run make fetch-check again\n' >&2
    exit 1
fi
reliefos_log OK "all $checked locked URLs answer"
