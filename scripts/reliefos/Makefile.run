# QEMU policy follows Kconfig; command-line variables override local defaults.
QEMU ?= qemu-system-x86_64
QEMU_FIRMWARE ?= $(or $(wildcard $(subst ",,$(CONFIG_QEMU_OVMF_PATH))),$(firstword $(wildcard /usr/share/edk2/x64/OVMF.4m.fd /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/qemu/OVMF.fd)))
QEMU_DISPLAY ?= gtk,grab-on-hover=on,show-cursor=on
QEMU_SERIAL ?= stdio
QEMU_AUDIO ?= sdl
QEMU_SOUND_DEVICE ?= hda
ifeq ($(QEMU_SOUND_DEVICE),hda)
QEMU_SOUND_FLAGS := -device intel-hda,msi=auto -device hda-duplex,audiodev=snd0
else ifeq ($(QEMU_SOUND_DEVICE),ac97)
QEMU_SOUND_FLAGS := -device AC97,audiodev=snd0
else ifeq ($(QEMU_SOUND_DEVICE),es1371)
QEMU_SOUND_FLAGS := -device ES1370,audiodev=snd0
else ifeq ($(QEMU_SOUND_DEVICE),none)
QEMU_SOUND_FLAGS :=
else
$(error QEMU_SOUND_DEVICE must be hda, ac97, es1371 or none)
endif
QEMU_KVM ?= $(if $(filter y,$(CONFIG_QEMU_ENABLE_KVM)),1,0)
RELIEFOS_QEMU_IDE ?= $(LEONOS_QEMU_IDE)
RELIEFOS_QEMU_NVME ?= $(LEONOS_QEMU_NVME)
QEMU_MEMORY := $(or $(MEMORY),$(CONFIG_QEMU_MEMORY_MB),4096)
QEMU_CPUS := $(or $(CPUS),$(CONFIG_QEMU_SMP_CPUS),4)
comma := ,
QEMU_NETWORK := $(subst ",,$(CONFIG_QEMU_NET_DEVICE))
QEMU_FLAGS := -machine $(if $(filter 1 y yes true,$(RELIEFOS_QEMU_IDE)),pc,q35) $(if $(filter 1,$(QEMU_KVM)),-enable-kvm -cpu host,-cpu max) -m $(QEMU_MEMORY) -smp $(QEMU_CPUS) -bios $(QEMU_FIRMWARE) -display $(QEMU_DISPLAY) -serial $(QEMU_SERIAL) -device VGA,xres=$(or $(CONFIG_QEMU_DISPLAY_WIDTH),1920),yres=$(or $(CONFIG_QEMU_DISPLAY_HEIGHT),1080) $(if $(QEMU_NETWORK),-netdev user,id=net0 -device $(QEMU_NETWORK)$(comma)netdev=net0) -audiodev $(QEMU_AUDIO),id=snd0 $(QEMU_SOUND_FLAGS)
QEMU_DISK := -drive file=$(DISK_VMDK),if=none,id=disk0,format=vmdk
ifneq ($(filter 1 y yes true,$(RELIEFOS_QEMU_NVME)),)
QEMU_DISK += -device nvme,drive=disk0,serial=leonosnvme
else ifneq ($(filter 1 y yes true,$(RELIEFOS_QEMU_IDE)),)
QEMU_DISK += -device piix3-ide,id=ide -device ide-hd,drive=disk0,bus=ide.0,unit=0
else
QEMU_DISK += -device ich9-ahci,id=ahci -device ide-hd,drive=disk0,bus=ahci.0
endif
.PHONY: run run-debug run-iso run-installer
run run-debug: image-vmdk
	$(Q)test -f '$(QEMU_FIRMWARE)' || { echo 'set QEMU_FIRMWARE to an OVMF firmware image' >&2; exit 1; }
	$(Q)$(QEMU) $(QEMU_FLAGS) $(QEMU_DISK) $(if $(filter run-debug,$@),-no-reboot -no-shutdown)
run-iso: iso
	$(Q)test -f '$(QEMU_FIRMWARE)'
	$(Q)$(QEMU) $(QEMU_FLAGS) -cdrom $(LIVE_ISO) -no-reboot -no-shutdown
run-installer: installer
	$(Q)test -f '$(QEMU_FIRMWARE)'
	$(Q)$(QEMU) $(QEMU_FLAGS) -cdrom $(INSTALLER_ISO) -no-reboot -no-shutdown
