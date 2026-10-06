# Boot chain products: consumer only.
#
# Since phase 3 of the kernel/userland separation the loader, the five .drv
# drivers and kerneldebug.sys are built by the standalone ReliefNT checkout and
# published to these legacy paths by the adapter in mk/kernel.mk. Nothing here
# compiles; the targets below are product-presence consumers that pull the
# published files into existence (and fail if the adapter cannot produce them).

LOADER_ELF := $(O_GENERATED)/boot/loader.elf
DRIVER_NAMES := mouse serial e1000 ac97 es1371 hda
DRIVER_OUTPUTS := $(addprefix $(O_GENERATED)/drivers/,$(addsuffix .drv,$(DRIVER_NAMES)))
KERNELDEBUG_SYS := $(O_GENERATED)/system/kerneldebug.sys

.PHONY: loader drivers boot
loader: $(LOADER_ELF)
drivers: $(DRIVER_OUTPUTS) $(KERNELDEBUG_SYS)
boot: kernel loader drivers
