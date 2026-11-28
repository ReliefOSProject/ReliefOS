# Official x86_64 Arch build userspace. Package signatures use the base image's
# pacman.conf and Arch keyring; never turn them off to work around a mirror.
FROM docker.io/library/archlinux@sha256:d4594bb2300cba11f41c2f27923eb56f63d9988909c402f9c95517d1e362c899
RUN pacman -Syu --noconfirm --needed gcc make clang lld llvm compiler-rt bison flex diffutils git \
    && pacman -Scc --noconfirm
