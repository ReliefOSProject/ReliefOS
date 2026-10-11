# Boot chain products: consumer only.
#
# Since phase 3 of the kernel/userland separation kerneldebug.sys is built by
# the standalone ReliefNT checkout and published to these legacy paths by the
# adapter in mk/kernel.mk. The device drivers are linked into kernel.sys itself
# and GRUB boots kernel.sys directly through its Multiboot2 header. Nothing
# here compiles; the targets below are product-presence consumers that pull the
# published files into existence (and fail if the adapter cannot produce them).

KERNELDEBUG_SYS := $(O_GENERATED)/system/kerneldebug.sys

.PHONY: kerneldebug boot
kerneldebug: $(KERNELDEBUG_SYS)
boot: kernel kerneldebug
