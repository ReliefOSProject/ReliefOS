# ReliefOS product Kbuild. Linux owns host/object actions and dependency files.
include $(srctree)/scripts/reliefos/Makefile.environment
include $(srctree)/scripts/reliefos/Makefile.parameters
include $(srctree)/scripts/reliefos/Makefile.logging
include $(srctree)/scripts/reliefos/Makefile.host
include $(srctree)/scripts/reliefos/Makefile.toolchain
include $(srctree)/scripts/reliefos/Makefile.config
include $(srctree)/scripts/reliefos/Makefile.kernel
include $(srctree)/scripts/reliefos/Makefile.headers
include $(srctree)/scripts/reliefos/Makefile.boot
include $(srctree)/scripts/reliefos/Makefile.third-party
include $(srctree)/scripts/reliefos/Makefile.pam
include $(srctree)/scripts/reliefos/Makefile.runtime
include $(srctree)/scripts/reliefos/Makefile.upstream
include $(srctree)/scripts/reliefos/Makefile.grub
include $(srctree)/scripts/reliefos/Makefile.components.audio
include $(srctree)/scripts/reliefos/Makefile.userland
include $(srctree)/scripts/reliefos/Makefile.resources
include $(srctree)/scripts/reliefos/Makefile.nls
include $(srctree)/scripts/reliefos/Makefile.sdk
include $(srctree)/scripts/reliefos/Makefile.rootfs
include $(srctree)/scripts/reliefos/Makefile.apk
include $(srctree)/scripts/reliefos/Makefile.images
include $(srctree)/scripts/reliefos/Makefile.rpr
include $(srctree)/scripts/reliefos/Makefile.site
include $(srctree)/scripts/reliefos/Makefile.run
include $(srctree)/scripts/reliefos/Makefile.tests
include $(srctree)/tools/host/Kbuild
tools: $(RELIEFOS_HOST_TOOLS)

kernel: $(RELIEFOS_KERNEL_SYS) $(RELIEFOS_KERNEL_DEBUG)

test: test-tools test-build

test-smoke: image-vmdk iso installer
	@QEMU='$(QEMU)' DUAL_BOOT_ACCEL='$(if $(filter 1,$(QEMU_KVM)),kvm,tcg)' DUAL_BOOT_CPU='$(if $(filter 1,$(QEMU_KVM)),host,max)' sh $(RELIEFOS_SRC)/scripts/test-smoke.sh $(O_IMAGES) $(O_LOGS) '$(QEMU_FIRMWARE)'

test-legacy:
	@sh $(RELIEFOS_SRC)/scripts/test-legacy.sh $(RELIEFOS_SRC)

rootfs: apk-repo

all: kernel userland runtime sdk apk-repo image-vmdk iso installer

.PHONY: image-iso release config-sync build-info test-all
image-iso: iso
# `release` is gated by the ReliefNT release guard (scripts/reliefos/Makefile.rpr): release builds
# must come from a clean kernel/reliefnt submodule at the committed gitlink.
release: reliefnt-release-guard all pages
config-sync: $(AUTOCONF_H) $(AUTOCONF_INSTALLER_H) $(RELIEFOS_COMPONENT_MK)
build-info: $(BUILD_INFO_HEADER)
test-all: test test-long test-legacy test-smoke
