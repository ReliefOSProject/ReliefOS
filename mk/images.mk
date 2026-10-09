# Host tools own the disk formats; Make owns each image dependency.
# Use the tracked, validated EFI modules. Host GRUB 2.14 has a relocator
# write-protection regression on UEFI Multiboot2; the host packaging utility
# can still assemble the pinned 2.12 kernel and modules.
GRUB_EFI_DIR ?= $(RELIEFOS_SRC)/boot/grub_modules_x86_64-efi
GRUB_EFI_INPUTS := $(wildcard $(GRUB_EFI_DIR)/*)
RELIEFOS_EXT2_TIME := $(RELIEFOS_HOST_BIN)/reliefos-ext2-time
RELIEFOS_EXT4_TIME := $(RELIEFOS_EXT2_TIME)
$(RELIEFOS_EXT2_TIME): $(O_HOST)/obj/tools/host/images/reliefos-ext2-time.c.o
	$(Q)$(HOSTCC) $(HOST_CFLAGS) $(HOST_LDFLAGS) $^ -lext2fs -lcom_err -o $@
ESP_STAGE := $(O_STAGE)/esp
ESP_STAMP := $(O_STAGE)/esp.complete
IMAGE_DISPLAY := $(O_GENERATED)/config/display.conf
$(IMAGE_DISPLAY): $(RELIEFOS_CONFIG_FILE) $(RELIEFOS_EMIT)
	$(Q)mkdir -p $(@D)
	$(Q)awk 'BEGIN{theme="metro";mode="fill"} /^CONFIG_VMDK_DEFAULT_THEME_WIN95=y$$/{theme="win95"} /^CONFIG_VMDK_WALLPAPER_STRETCH=y$$/{mode="stretch"} /^CONFIG_VMDK_WALLPAPER_CENTER=y$$/{mode="center"} END{print "theme="theme;print "wallpaper.mode="mode}' $< | $(RELIEFOS_EMIT) --input - --output $@
RELIEFOS_EXT4_FEATURES := none,filetype,extents,dir_index,metadata_csum,64bit,flex_bg,has_journal,large_file,huge_file,extra_isize
RELIEFOS_SIG_images := epoch=$(SOURCE_DATE_EPOCH)|root-fs=ext4|ext4-features=$(RELIEFOS_EXT4_FEATURES)|grub=$(GRUB_EFI_DIR)|grub-identity=$(shell grub-mkstandalone --version 2>/dev/null)|mke2fs=$(shell mke2fs -V 2>&1 | head -n1)|mtools=$(shell mcopy -V 2>&1 | head -n1)|disk-size=$(CONFIG_IMAGE_SIZE_MIB)|installer-size=$(CONFIG_INSTALLER_ROOT_SIZE_MIB)|qemu-img=$(shell qemu-img --version 2>/dev/null | head -n1)|xorriso=$(shell xorriso -version 2>/dev/null | head -n1)|sfdisk=$(shell sfdisk --version 2>/dev/null)
$(if $(RELIEFOS_PASSIVE),,$(eval $(call RELIEFOS_SIGNATURE_RULE,images)))
$(ESP_STAMP): $(LOADER_ELF) $(O_GENERATED)/system/kernel.sys $(GRUB_FONT) $(IMAGE_DISPLAY) $(RELIEFOS_SRC)/tools/build/efi-stage.sh $(RELIEFOS_SRC)/boot/grub/embedded.cfg $(RELIEFOS_SRC)/boot/grub/grub.cfg $(RELIEFOS_SRC)/boot/grub/theme/theme.txt $(GRUB_EFI_INPUTS) $(O_META)/images.sig
	$(Q)sh $(RELIEFOS_SRC)/tools/build/efi-stage.sh $(RELIEFOS_SRC) $(GRUB_EFI_DIR) $(LOADER_ELF) $(O_GENERATED)/system/kernel.sys $(GRUB_FONT) $(IMAGE_DISPLAY) $(ESP_STAGE) $(SOURCE_DATE_EPOCH)
	$(Q)touch $@
	$(Q)sh $(RELIEFOS_SRC)/tools/build/stage-inventory.sh write $(ESP_STAGE) $(O_META)/esp.files
.PHONY: esp
esp: $(ESP_STAMP)
tools: $(RELIEFOS_EXT2_TIME)

ROOT_EXT2 := $(O_IMAGES)/root.ext2
DISK_ROOT_EXT2 := $(O_IMAGES)/disk-root.ext2
ROOT_EXT4 := $(O_IMAGES)/root.ext4
DISK_ROOT_EXT4 := $(O_IMAGES)/disk-root.ext4
LIVE_ROOT_STAGE := $(O_STAGE)/live-root
DISK_ROOT_STAGE := $(O_STAGE)/disk-root
define RELIEFOS_STANDALONE_STAGE
$(O_STAGE)/$(1)-root.complete: $(APK_MANAGED_ROOT)/.apk-complete $(RELIEFOS_SRC)/tools/build/standalone-root.sh $(wildcard $(RELIEFOS_SRC)/system/test-accounts/*) $(RELIEFOS_CONFIG_FILE)
	$$(Q)sh $$(RELIEFOS_SRC)/tools/build/standalone-root.sh $$(RELIEFOS_SRC) $$(APK_MANAGED_ROOT) $$(O_STAGE)/$(1)-root $(1) '$$(KCONFIG_CONFIG_VMDK_DEFAULT_LANG)'
	$$(Q)touch $$@
	$$(Q)sh $$(RELIEFOS_SRC)/tools/build/stage-inventory.sh write $$(O_STAGE)/$(1)-root $$(O_META)/$(1)-root.files
endef
$(eval $(call RELIEFOS_STANDALONE_STAGE,live))
$(eval $(call RELIEFOS_STANDALONE_STAGE,disk))
INSTALLER_ROOT := $(O_STAGE)/installer
INSTALLER_STAGE_STAMP := $(O_STAGE)/installer.complete
INSTALLER_EXT2 := $(O_IMAGES)/installer-root.ext2
INSTALLER_EXT4 := $(O_IMAGES)/installer-root.ext4
DISK_RAW := $(O_IMAGES)/reliefos.raw
DISK_VMDK := $(O_IMAGES)/reliefos.vmdk
LIVE_ISO := $(O_IMAGES)/reliefos-live.iso
INSTALLER_ISO := $(O_IMAGES)/reliefos-installer.iso
$(LIVE_ISO) $(INSTALLER_ISO): $(BUILD_LOG_INPUTS)
$(INSTALLER_STAGE_STAMP): $(USERLAND_DIR)/installer.elf
$(ROOT_EXT2): $(O_STAGE)/live-root.complete $(RELIEFOS_EXT2_TIME) $(RELIEFOS_SRC)/tools/build/images.sh $(O_META)/images.sig
	$(Q)RELIEFOS_EXT2_TIME=$(RELIEFOS_EXT2_TIME) sh $(RELIEFOS_SRC)/tools/build/images.sh ext2 $(LIVE_ROOT_STAGE) $@ $(SOURCE_DATE_EPOCH) 5c13543b-732c-4f81-8652-621124484420
$(DISK_ROOT_EXT2): $(O_STAGE)/disk-root.complete $(RELIEFOS_EXT2_TIME) $(RELIEFOS_SRC)/tools/build/images.sh $(O_META)/images.sig
	$(Q)RELIEFOS_EXT2_TIME=$(RELIEFOS_EXT2_TIME) sh $(RELIEFOS_SRC)/tools/build/images.sh ext2 $(DISK_ROOT_STAGE) $@ $(SOURCE_DATE_EPOCH) 5c13543b-732c-4f81-8652-621124484420
$(ROOT_EXT4): $(O_STAGE)/live-root.complete $(RELIEFOS_EXT4_TIME) $(RELIEFOS_SRC)/tools/build/images.sh $(O_META)/images.sig
	$(Q)RELIEFOS_EXT4_TIME=$(RELIEFOS_EXT4_TIME) sh $(RELIEFOS_SRC)/tools/build/images.sh ext4 $(LIVE_ROOT_STAGE) $@ $(SOURCE_DATE_EPOCH) 5c13543b-732c-4f81-8652-621124484420
$(DISK_ROOT_EXT4): $(O_STAGE)/disk-root.complete $(RELIEFOS_EXT4_TIME) $(RELIEFOS_SRC)/tools/build/images.sh $(O_META)/images.sig
	$(Q)RELIEFOS_EXT4_TIME=$(RELIEFOS_EXT4_TIME) sh $(RELIEFOS_SRC)/tools/build/images.sh ext4 $(DISK_ROOT_STAGE) $@ $(SOURCE_DATE_EPOCH) 5c13543b-732c-4f81-8652-621124484420
RELIEFOS_DEDUP_TOOL := $(RELIEFOS_HOST_BIN)/reliefos-dedup
$(RELIEFOS_DEDUP_TOOL): $(O_HOST)/obj/tools/host/manifest/reliefos-dedup.c.o | $(RELIEFOS_HOST_BIN)
	$(Q)$(HOSTCC) $(HOST_CFLAGS) $(HOST_LDFLAGS) $^ -o $@
tools: $(RELIEFOS_DEDUP_TOOL)
$(INSTALLER_STAGE_STAMP): $(RELIEFOS_DEDUP_TOOL) $(APK_STAGE_INPUTS)
$(INSTALLER_STAGE_STAMP): $(ROOTFS_RAW_STAMP) $(ESP_STAMP) $(USERLAND_DIR)/installer.elf $(O)/userland-installer/gptinit.elf $(APK_MANAGED_ROOT)/.apk-complete $(RELIEFOS_SRC)/tools/build/installer-stage.sh $(RELIEFOS_SRC)/system/xorg/installer-session $(RELIEFOS_SRC)/userland/apps/installer/installer.desktop $(APK_STAGE_SCRIPT) $(RELIEFOS_SRC)/docs/ADVANCED_INSTALL.txt $(O_META)/images.sig
	$(Q)mkdir -p $(O_LOGS)
	$(Q)APK_TOOL=$(abspath $(UPSTREAM_APK)) APK_UPSTREAM=$(abspath $(UPSTREAM_APK_ROOT)) APK_OWN_TOOL=$(abspath $(RELIEFOS_APK_OWN)) APK_KEY='$(APK_SIGNING_KEY)' APK_VERSION='$(APK_BUILD_VERSION)' sh $(RELIEFOS_SRC)/tools/build/run-logged.sh $(O_LOGS)/installer-stage.log sh $(RELIEFOS_SRC)/tools/build/installer-stage.sh $(RELIEFOS_SRC) $(abspath $(O)) $(abspath $(ROOTFS_RAW)) $(abspath $(ESP_STAGE)) $(abspath $(INSTALLER_ROOT)) $(SOURCE_DATE_EPOCH)
	$(Q)touch $@
	$(Q)sh $(RELIEFOS_SRC)/tools/build/stage-inventory.sh write $(INSTALLER_ROOT) $(O_META)/installer.files
$(INSTALLER_EXT2): $(INSTALLER_STAGE_STAMP) $(RELIEFOS_EXT2_TIME) $(RELIEFOS_SRC)/tools/build/images.sh $(O_META)/images.sig
	$(Q)IMAGE_MIN_MIB=$(or $(CONFIG_INSTALLER_ROOT_SIZE_MIB),400) RELIEFOS_EXT2_TIME=$(RELIEFOS_EXT2_TIME) sh $(RELIEFOS_SRC)/tools/build/images.sh ext2 $(INSTALLER_ROOT) $@ $(SOURCE_DATE_EPOCH) 9e1f6d46-3f41-42d2-9917-9a21a2134401
$(INSTALLER_EXT4): $(INSTALLER_STAGE_STAMP) $(RELIEFOS_EXT4_TIME) $(RELIEFOS_SRC)/tools/build/images.sh $(O_META)/images.sig
	$(Q)IMAGE_MIN_MIB=$(or $(CONFIG_INSTALLER_ROOT_SIZE_MIB),400) RELIEFOS_EXT4_TIME=$(RELIEFOS_EXT4_TIME) sh $(RELIEFOS_SRC)/tools/build/images.sh ext4 $(INSTALLER_ROOT) $@ $(SOURCE_DATE_EPOCH) 9e1f6d46-3f41-42d2-9917-9a21a2134401
$(DISK_RAW) $(DISK_VMDK) &: $(DISK_ROOT_EXT4) $(ESP_STAMP) $(RELIEFOS_SRC)/tools/build/disk.sh $(O_META)/images.sig
	$(Q)IMAGE_MIN_MIB=$(or $(CONFIG_IMAGE_SIZE_MIB),1024) sh $(RELIEFOS_SRC)/tools/build/disk.sh $(ESP_STAGE) $(DISK_ROOT_EXT4) $(DISK_RAW) $(DISK_VMDK) $(SOURCE_DATE_EPOCH)
$(LIVE_ISO): $(ROOT_EXT4) $(ESP_STAMP) $(RELIEFOS_SRC)/tools/build/iso.sh $(RELIEFOS_SRC)/boot/grub/live.cfg $(RELIEFOS_SRC)/boot/grub/installer_embedded.cfg $(O_META)/images.sig
	$(Q)sh $(RELIEFOS_SRC)/tools/build/iso.sh $(RELIEFOS_SRC) $(GRUB_EFI_DIR) $(ESP_STAGE) $(ROOT_EXT4) $(RELIEFOS_SRC)/boot/grub/live.cfg $@ $(SOURCE_DATE_EPOCH) RELIEFOSLIVE
$(INSTALLER_ISO): $(INSTALLER_EXT4) $(ESP_STAMP) $(RELIEFOS_SRC)/tools/build/iso.sh $(RELIEFOS_SRC)/boot/grub/installer.cfg $(RELIEFOS_SRC)/boot/grub/installer_embedded.cfg $(O_META)/images.sig
	$(Q)sh $(RELIEFOS_SRC)/tools/build/iso.sh $(RELIEFOS_SRC) $(GRUB_EFI_DIR) $(ESP_STAGE) $(INSTALLER_EXT4) $(RELIEFOS_SRC)/boot/grub/installer.cfg $@ $(SOURCE_DATE_EPOCH) RELIEFOSINST
.PHONY: image-vmdk iso installer
image-vmdk: $(DISK_RAW) $(DISK_VMDK)
iso: $(LIVE_ISO)
installer: $(INSTALLER_ISO)

define RELIEFOS_IMAGE_PRESENCE
$(O_META)/$(1)-present.sig: FORCE
	$$(Q)sh $$(RELIEFOS_SRC)/tools/build/stage-inventory.sh check $(2) $$(O_META)/$(1).files $$@
$(3): $(O_META)/$(1)-present.sig $(RELIEFOS_SRC)/tools/build/stage-inventory.sh
endef
$(eval $(call RELIEFOS_IMAGE_PRESENCE,esp,$(ESP_STAGE),$(ESP_STAMP)))
$(eval $(call RELIEFOS_IMAGE_PRESENCE,live-root,$(LIVE_ROOT_STAGE),$(O_STAGE)/live-root.complete))
$(eval $(call RELIEFOS_IMAGE_PRESENCE,disk-root,$(DISK_ROOT_STAGE),$(O_STAGE)/disk-root.complete))
$(eval $(call RELIEFOS_IMAGE_PRESENCE,installer,$(INSTALLER_ROOT),$(INSTALLER_STAGE_STAMP)))
