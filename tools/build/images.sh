#!/bin/sh
# Rootless filesystem/ISO adapter. External tools own disk formats.
set -eu
[ "$#" -ge 1 ] || exit 2
mode=$1; shift
case $mode in
ext2)
    [ "$#" = 4 ] || exit 2
    stage=$1 output=$2 epoch=$3 uuid=$4
    [ -d "$stage" ] && [ -n "$output" ] && [ -n "$uuid" ]
    mkdir -p "$(dirname "$output")"
    work=$(mktemp -d "$output.work.XXXXXX")
    trap 'rm -rf "$work"' EXIT HUP INT TERM
    # Copies prevent ownership/mode normalization from changing the input stage.
    cp -a "$stage" "$work/root"
    find "$work/root" -print0 | xargs -0 touch -h -d "@$epoch"
    count=$(find "$work/root" -printf '.\n' | wc -l)
    bytes=$(du -sb "$work/root" | cut -f1)
    mib=$(( (bytes + count * 8192 + 67108864 + 33554431) / 33554432 * 32 ))
    [ "$mib" -ge 128 ] || mib=128
    minimum=${IMAGE_MIN_MIB:-0}
    case $minimum in ''|*[!0-9]*) echo 'IMAGE_MIN_MIB must be an integer' >&2; exit 2;; esac
    [ "$mib" -ge "$minimum" ] || mib=$minimum
    inodes=$((count * 2)); [ "$inodes" -ge 8192 ] || inodes=8192
    truncate -s "${mib}M" "$work/root.ext2"
    # fakeroot intercepts chown/stat; real host privileges are unnecessary.
    E2FSPROGS_FAKE_TIME="$epoch" FAKEROOTDONTTRYCHOWN=1 fakeroot -- sh -eu -c '
        find "$1" -exec chown -h 0:0 {} +
        if [ -f "$1/etc/reliefos/test-image" ]; then
            test "$(cat "$1/etc/reliefos/test-image")" = leonos-standalone-test-v1
            test ! -L "$1/home/test"
            find "$1/home/test" -exec chown -h 1000:1000 {} +
        fi
        mke2fs -q -t ext2 -F -b 4096 -I 128 -O none,filetype,sparse_super,large_file -m 0 -E root_owner=0:0,hash_seed="$4" -U "$4" -N "$3" -d "$1" "$2"
    ' sh "$work/root" "$work/root.ext2" "$inodes" "$uuid"
    reliefos_ext2_time=${RELIEFOS_EXT2_TIME:-${LEONOS_EXT2_TIME:-}}
    : "${reliefos_ext2_time:?ext2 timestamp tool required}"
    "$reliefos_ext2_time" "$work/root.ext2" "$epoch"
    e2fsck -f -n "$work/root.ext2"
    mv "$work/root.ext2" "$output"
    ;;
ext4)
    [ "$#" = 4 ] || exit 2
    stage=$1 output=$2 epoch=$3 uuid=$4
    [ -d "$stage" ] && [ -n "$output" ] && [ -n "$uuid" ]
    mkdir -p "$(dirname "$output")"
    work=$(mktemp -d "$output.work.XXXXXX")
    trap 'rm -rf "$work"' EXIT HUP INT TERM
    cp -a "$stage" "$work/root"
    find "$work/root" -print0 | xargs -0 touch -h -d "@$epoch"
    count=$(find "$work/root" -printf '.\n' | wc -l)
    bytes=$(du -sb "$work/root" | cut -f1)
    mib=$(( (bytes + count * 8192 + 67108864 + 33554431) / 33554432 * 32 ))
    [ "$mib" -ge 128 ] || mib=128
    minimum=${IMAGE_MIN_MIB:-0}
    case $minimum in ''|*[!0-9]*) echo 'IMAGE_MIN_MIB must be an integer' >&2; exit 2;; esac
    [ "$mib" -ge "$minimum" ] || mib=$minimum
    inodes=$((count * 2)); [ "$inodes" -ge 8192 ] || inodes=8192
    truncate -s "${mib}M" "$work/root.ext4"
    E2FSPROGS_FAKE_TIME="$epoch" FAKEROOTDONTTRYCHOWN=1 fakeroot -- sh -eu -c '
        find "$1" -exec chown -h 0:0 {} +
        if [ -f "$1/etc/reliefos/test-image" ]; then
            test "$(cat "$1/etc/reliefos/test-image")" = leonos-standalone-test-v1
            test ! -L "$1/home/test"
            find "$1/home/test" -exec chown -h 1000:1000 {} +
        fi
        mke2fs -q -t ext4 -F -b 4096 -I 256 -O none,filetype,extents,dir_index,metadata_csum,64bit,flex_bg,has_journal,large_file,huge_file,extra_isize -m 0 -E root_owner=0:0,hash_seed="$4" -U "$4" -N "$3" -d "$1" "$2"
    ' sh "$work/root" "$work/root.ext4" "$inodes" "$uuid"
    reliefos_ext4_time=${RELIEFOS_EXT4_TIME:-${RELIEFOS_EXT2_TIME:-${LEONOS_EXT2_TIME:-}}}
    : "${reliefos_ext4_time:?ext4 timestamp tool required}"
    "$reliefos_ext4_time" "$work/root.ext4" "$epoch"
    e2fsck -f -n "$work/root.ext4"
    mv "$work/root.ext4" "$output"
    ;;
*) echo "unknown image operation: $mode" >&2; exit 2 ;;
esac
