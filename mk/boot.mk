# Boot chain products: consumer only.
#
# Since phase 3 of the kernel/userland separation the loader and kerneldebug.sys
# are built by the standalone ReliefNT checkout and published to these legacy
# paths by the adapter in mk/kernel.mk. The device drivers are linked into
# kernel.sys itself. Nothing here compiles; the targets below are
# product-presence consumers that pull the published files into existence (and
# fail if the adapter cannot produce them).

LOADER_ELF := $(O_GENERATED)/boot/loader.elf
KERNELDEBUG_SYS := $(O_GENERATED)/system/kerneldebug.sys

.PHONY: loader kerneldebug boot
loader: $(LOADER_ELF)
kerneldebug: $(KERNELDEBUG_SYS)
boot: kernel loader kerneldebug
