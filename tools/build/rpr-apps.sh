#!/bin/sh
# Optional official application packages; all cryptography/formats use apk.
set -eu
[ "$#" = 8 ] || exit 2
src=$1 out=$2 build=$3 index=$4 apk=$5 key=$6 output=$7 epoch=$8
export SOURCE_DATE_EPOCH=$epoch
version=$(sed -n 's/^#define RELIEFOS_KERNEL_VERSION "\([0-9.]*\)"$/\1/p' "$build")
printf '%s\n' "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || { echo 'invalid release version' >&2; exit 1; }
case $epoch in ''|*[!0-9]*) echo 'invalid package epoch' >&2; exit 1;; esac
# APK revisions retain chronological ordering against previously published
# packages. This is packaging metadata, not a kernel build number or counter.
package_version=$version-r$epoch
mkdir -p "$(dirname "$output")"
work=$(mktemp -d "$output.new.XXXXXX")
work=$(CDPATH= cd -- "$work" && pwd -P)
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir "$work/repository"
manifest() {
    printf '[app]\nid=%s\nname=%s\nversion=%s\ncategory=%s\nexec=%s\nicon=%s\nentry=%s\nterminal=0\nsystem=0\nhidden=0\nopen_with=0\ncommands=%s\nextensions=\n' "$1" "$2" "$version" "$3" "$4" "$5" "$6" "$7" > "$root/manifest.ini"
}
for app in helloworld doom oschinpt; do
    payload=$work/$app
    root=$payload/usr/lib/reliefos/apps/$app
    mkdir -p "$root" "$payload/usr/bin"
    cp "$out/userland/$app.elf" "$root/$app.elf"
    case $app in
    helloworld)
        cp "$src/resources/build-art/app-icons/helloworld.bmp" "$root/"
        manifest helloworld 'Hello World' 'Developer applications' helloworld.elf helloworld.bmp 1 helloworld
        ln -s ../lib/reliefos/apps/helloworld/helloworld.elf "$payload/usr/bin/helloworld"
        ;;
    doom)
        cp "$out/userland/doomlauncher.elf" "$root/"
        cp "$src/third_party/doomgeneric/freedoom1.wad" "$src/third_party/doomgeneric/FREEDOOM-COPYING.txt" "$root/"
        cp "$src/third_party/doomgeneric/LICENSE" "$root/DOOMGENERIC-LICENSE"
        manifest doom DOOM Games doomlauncher.elf '' 1 doom,doomlauncher
        ln -s ../lib/reliefos/apps/doom/doomlauncher.elf "$payload/usr/bin/doom"
        ln -s ../lib/reliefos/apps/doom/doomlauncher.elf "$payload/usr/bin/doomlauncher"
        ;;
    oschinpt)
        cp "$src/third_party/rime-pinyin-simp/pinyin_simp.dict.yaml" "$src/third_party/rime-pinyin-simp/LICENSE" "$src/third_party/rime-pinyin-simp/ATTRIBUTION.txt" "$root/"
        cp "$src/userland/apps/oschinpt/settings.ini" "$root/"
        cp "$index" "$root/oscp.idx"
        manifest oschinpt 'ReliefOS Chinese Input' 'Input methods' oschinpt.elf '' 0 oschinpt
        printf 'input_method=1\n' >> "$root/manifest.ini"
        ln -s ../lib/reliefos/apps/oschinpt/oschinpt.elf "$payload/usr/bin/oschinpt"
        ;;
    esac
    find "$payload" -exec touch -h -d "@$epoch" {} +
    package_file="$work/repository/reliefos-$app-$package_version.apk"
    set -- mkpkg --files "$payload" --output "$package_file" --info "name:reliefos-$app" \
      --info "version:$package_version" --info arch:x86_64 --info "origin:reliefos-$app" \
      --info "description:ReliefOS application $app" --info license:LicenseRef-See-Bundled-Notices \
      --info 'depends:reliefos-apps reliefos-musl' --info "replaces:leonos-$app" --sign-key "$key"
    if [ "$app" = oschinpt ]; then
        set -- "$@" --script "post-install:$src/tools/oschinpt-apk-post-install" \
          --script "post-upgrade:$src/tools/oschinpt-apk-post-install" \
          --script "post-deinstall:$src/tools/oschinpt-apk-post-deinstall"
    fi
    if unshare -Ur true >/dev/null 2>&1; then unshare -Ur "$apk" "$@"
    else fakeroot "$apk" "$@"
    fi
    printf '%s\n' "${package_file##*/}" >> "$work/repository/packages.list"
done
printf '%s\n' "$version" > "$work/repository/.complete"
if [ -d "$output.previous" ] && [ ! -e "$output" ]; then mv "$output.previous" "$output"; fi
rm -rf "$output.previous"
if [ -e "$output" ]; then mv "$output" "$output.previous"; fi
mv "$work/repository" "$output"
rm -rf "$output.previous"
