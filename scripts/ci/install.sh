#!/usr/bin/env bash
set -euo pipefail

case "${1:-}" in
  build)
    packages=(
      build-essential clang libclang-rt-dev llvm lld ccache bison flex gperf gettext
      grub-efi-amd64-bin grub-pc-bin grub-common xorriso mtools dosfstools
      e2fsprogs squashfs-tools libext2fs-dev fdisk zip openssl fakeroot gdisk qemu-utils
      git make perl libncurses-dev bc pkg-config autoconf automake libtool
      file curl patch xz-utils iproute2
      libtool-bin gawk ncurses-bin texinfo exfatprogs python3-sphinx python3-pil
      qemu-system-x86 ovmf
      xvfb xauth libx11-dev libpng-dev libcairo2-dev libfreetype6-dev
      libglib2.0-dev ffmpeg libavformat-dev libavcodec-dev libavutil-dev
      libswresample-dev libswscale-dev
    )
    ;;
  kernel|auth|pages)
    # Focused suites build native tools and disposable kernel/runtime fixtures;
    # they do not launch guests or compile the desktop/media applications.
    packages=(
      build-essential clang libclang-rt-dev llvm lld ccache bison flex gperf gettext
      git make perl libncurses-dev bc pkg-config autoconf automake libtool libtool-bin
      gawk ncurses-bin texinfo curl file patch xz-utils openssl python3 libext2fs-dev
    )
    if [[ "$1" == pages ]]; then
      packages+=(python3-sphinx python3-pil)
    fi
    ;;
  smoke)
    packages=(qemu-system-x86 qemu-utils ovmf python3-pil tesseract-ocr)
    ;;
  *) echo 'Usage: install.sh build|kernel|auth|pages|smoke' >&2; exit 2 ;;
esac

sudo apt-get update
sudo apt-get install --yes --no-install-recommends "${packages[@]}"
