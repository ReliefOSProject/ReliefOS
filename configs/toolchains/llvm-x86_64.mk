# ReliefOS x86_64 target toolchain description.
#
# Reviewed data, not logic: scripts/reliefos/Makefile.toolchain interprets it and `make doctor`
# verifies it. Switching toolchains means pointing TOOLCHAIN= at another file in
# this directory, never at an undeclared environment variable.

TOOLCHAIN_NAME := llvm-x86_64

# The kernel and the boot loader link at fixed addresses with
# ld.lld; userland targets x86_64-linux-musl against the project sysroot.
TRIPLE_KERNEL := x86_64-unknown-none
TRIPLE_USER := x86_64-linux-musl

TOOLCHAIN_CC := clang
TOOLCHAIN_CXX := clang++
TOOLCHAIN_AR := llvm-ar
TOOLCHAIN_RANLIB := llvm-ranlib
TOOLCHAIN_LD := ld.lld
TOOLCHAIN_OBJCOPY := llvm-objcopy
TOOLCHAIN_STRIP := llvm-strip

# Checked by `make doctor`: target C, C++, assembly and LLVM binary tools.
TOOLCHAIN_REQUIRED := $(TOOLCHAIN_CC) $(TOOLCHAIN_CXX) $(TOOLCHAIN_AR) \
	$(TOOLCHAIN_LD) $(TOOLCHAIN_OBJCOPY) $(TOOLCHAIN_STRIP) $(TOOLCHAIN_RANLIB)
