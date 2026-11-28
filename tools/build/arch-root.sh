#!/bin/bash
# Provision a signed Arch toolchain, then run the existing Kbuild as the caller.
set -euo pipefail

fail() {
    printf 'arch-root: %s\n' "$*" >&2
    exit 1
}

operation=$1
source_root=$2
requested_output=$3
profile=$4
container_output=/reliefos-build
recipe=$source_root/tools/build/arch-root.Dockerfile

for tool in docker git uname id nproc realpath sha256sum flock ls mkdir; do
    command -v "$tool" >/dev/null || fail "missing host tool: $tool (see Documentation/kbuild/building.rst)"
done
[[ $(uname -s):$(uname -m) == Linux:x86_64 ]] || fail 'an x86_64 Linux host is required'
uid=$(id -u)
gid=$(id -g)
[[ $uid != 0 ]] || fail 'run make as an ordinary user with Docker access, not via sudo'
jobs=${ARCH_JOBS:-$(nproc)}
[[ $jobs =~ ^[1-9][0-9]*$ ]] || fail 'ARCH_JOBS must be a positive integer'
[[ $profile == release || $profile == debug ]] || fail 'PROFILE must be release or debug'

endpoint=$(docker context inspect --format '{{.Endpoints.docker.Host}}') || fail 'cannot inspect the Docker context'
[[ $endpoint == unix:///* ]] || fail "a local Docker Unix socket is required (found $endpoint)"
engine=$(docker info --format '{{println .ServerVersion}}{{range .SecurityOptions}}{{println .}}{{end}}') || fail 'cannot reach Docker; check local daemon and socket permissions'
version=${engine%%$'\n'*}
[[ $version =~ ^([0-9]+)\. && ${BASH_REMATCH[1]} -ge 25 ]] || fail "Docker Engine 25 or later is required (found $version)"
case $engine in
    *name=rootless*|*name=userns*) fail 'Docker must preserve the caller UID/GID on bind mounts (rootless/userns-remap are not supported)' ;;
esac

valid_bind_path() {
    case $1 in
        *[!a-zA-Z0-9_./-]*) fail "unsupported bind path: $1" ;;
    esac
}

valid_bind_path "$source_root"
kernel=$source_root/kernel/reliefnt
[[ -f $kernel/Makefile && -e $kernel/.git ]] || fail 'initialize kernel/reliefnt with git submodule update --init --recursive'

# Git version queries also need the common dir of a linked worktree (and a
# separate gitdir for an absorbed submodule). Keep the checkout's absolute
# paths intact so .git files and core.worktree still resolve inside the root.
read_only=("$source_root")
add_read_only_mount() {
    local path mounted
    path=$(realpath -e -- "$1") || fail "cannot resolve Git storage: $1"
    valid_bind_path "$path"
    for mounted in "${read_only[@]}"; do
        [[ $path/ != "$mounted/"* ]] || return 0
    done
    read_only+=("$path")
}
for checkout in "$source_root" "$kernel"; do
    for kind in --git-common-dir --absolute-git-dir; do
        git_dir=$(git -C "$checkout" rev-parse --path-format=absolute "$kind") || fail "cannot resolve Git metadata for $checkout"
        add_read_only_mount "$git_dir"
    done
    # Shared clones can also borrow objects from other repositories. Git
    # resolves this list transitively, including relative alternate paths.
    object_stores=$(git -C "$checkout" count-objects -v) || fail "cannot inspect Git object stores for $checkout"
    while IFS= read -r line; do
        case $line in 'alternate: '*) add_read_only_mount "${line#alternate: }" ;; esac
    done <<< "$object_stores"
done

output=$(realpath -m -- "$requested_output")
[[ $output == "$(realpath -ms -- "$requested_output")" ]] || fail 'refusing a symlink in the output path'
valid_bind_path "$output"
[[ $output != / && $source_root/ != "$output/"* ]] || fail 'output cannot contain the source checkout'
if [[ $output/ == "$source_root/"* && $output != "$source_root/out/"* ]]; then
    fail 'in-tree output must be a directory below out/'
fi
for mounted in "${read_only[@]:1}" "$source_root/.git"; do
    [[ $output/ != "$mounted/"* && $mounted/ != "$output/"* ]] || fail 'output cannot overlap Git metadata'
done

marker=$output/.reliefos-arch-root
owner="reliefos-arch-root version=1 root=$source_root"
check_output() {
    [[ ! -L $marker ]] || fail 'refusing a symlinked Arch output marker'
    if [[ -f $marker ]]; then
        [[ $(<"$marker") == "$owner" ]] || fail 'output belongs to another Arch build checkout'
    elif [[ -e $output ]]; then
        local contents
        [[ -d $output ]] || fail "output is not a directory: $output"
        contents=$(ls -A -- "$output") || fail "cannot inspect output: $output"
        [[ -z $contents ]] || fail "output is not an empty or owned Arch build directory: $output"
    fi
}
check_output
if [[ $operation == doctor ]]; then
    printf 'arch-root: host ready (UID:GID %s:%s, output %s)\n' "$uid" "$gid" "$output"
    exit 0
fi

recipe_id=$(sha256sum "$recipe")
image=reliefos-arch-build:${recipe_id%% *}
if ! docker image inspect "$image" >/dev/null 2>&1; then
    [[ $operation == fetch ]] || fail "toolchain image is missing; run make O=$requested_output arch-fetch"
    # Dockerfile on stdin: no checkout, credentials, or build output in the context.
    docker build --platform=linux/amd64 --tag "$image" - < "$recipe"
fi

mkdir -p -- "$output"
# This lock lives on the host and spans the container invocation. The native
# Kbuild locks inside the root still own the inner Make invocations.
exec 9< "$output"
flock -n 9 || fail "output is already in use: $output"
check_output
if [[ ! -f $marker ]]; then
    printf '%s\n' "$owner" > "$marker"
fi

mounts=()
for path in "${read_only[@]}"; do
    mounts+=(--mount "type=bind,src=$path,dst=$path,readonly,bind-recursive=disabled")
done
mounts+=(--mount "type=bind,src=$output,dst=$container_output,bind-recursive=disabled")

case $operation in
    fetch) network=bridge; goal=reliefnt-fetch ;;
    build) network=none; goal=kernel ;;
    *) fail "unknown operation: $operation" ;;
esac

docker run --rm --pull=never --platform=linux/amd64 --read-only \
    --cap-drop=ALL --security-opt=no-new-privileges \
    --tmpfs /tmp:rw,nosuid,nodev,mode=1777 --env HOME=/tmp --env GIT_OPTIONAL_LOCKS=0 \
    --env SOURCE_DATE_EPOCH --env V --user "$uid:$gid" --network "$network" \
    "${mounts[@]}" --workdir "$source_root" "$image" \
    make --no-print-directory -j"$jobs" O="$container_output" ARCH=x86_64 PROFILE="$profile" \
        RELIEFNT_O="$container_output/reliefnt" RELIEFNT_CACHE="$container_output/downloads/reliefnt" "$goal"
