# Rootfs composition is driven by source and installed-product inventories.
RELIEFOS_STAGE_TOOL := $(RELIEFOS_HOST_BIN)/reliefos-stage
RELIEFOS_LAYOUT_TOOL := $(RELIEFOS_HOST_BIN)/reliefos-layout
$(RELIEFOS_STAGE_TOOL): $(O_HOST)/obj/tools/host/manifest/reliefos-stage.c.o $(RELIEFOS_HOST_COMMON_OBJS) $(O_HOST)/obj/tools/host/manifest/json.c.o
	$(Q)$(HOSTCC) $(HOST_CFLAGS) $(HOST_LDFLAGS) $^ -o $@
$(RELIEFOS_LAYOUT_TOOL): $(O_HOST)/obj/tools/host/manifest/reliefos-layout.c.o
	$(Q)$(HOSTCC) $(HOST_CFLAGS) $(HOST_LDFLAGS) $^ -o $@
ROOTFS_RAW := $(O)/rootfs/raw
ROOTFS_RAW_STAMP := $(O)/rootfs/raw.complete
ROOTFS_MANIFEST := $(O)/rootfs/manifest.json
APK_RAW_ROOT := $(ROOTFS_RAW)
APK_RAW_STAMP := $(ROOTFS_RAW_STAMP)
ROOTFS_SOURCE_DIRS := $(RELIEFOS_SRC)/system $(RELIEFOS_SRC)/resources/build-art $(RELIEFOS_SRC)/userland/storage $(RELIEFOS_SRC)/userland/apps
ROOTFS_SOURCE_DIRS += $(RELIEFOS_SRC)/userland/fastfetch $(RELIEFOS_SRC)/logo.png $(RELIEFOS_SRC)/test/test.mp3 $(RELIEFOS_SRC)/docs/APK_PREPARATION.md $(RELIEFOS_SRC)/configs/apk-ownership.json $(RELIEFOS_SRC)/third_party/stardustui/docs/zh-cn/example
ROOTFS_SOURCE_DIRS += $(wildcard $(addprefix $(RELIEFOS_SRC)/third_party/,busybox/LICENSE sl/LICENSE pl_editor/LICENSE portablegl/LICENSE))
ROOTFS_SOURCE_DIRS += $(RELIEFOS_SRC)/tools/build/rpr-config.sh
$(O_META)/rootfs-sources.sig: FORCE $(RELIEFOS_SRC)/tools/build/tree-signature.sh
	$(Q)sh $(RELIEFOS_SRC)/tools/build/tree-signature.sh $@ $(ROOTFS_SOURCE_DIRS)
ROOTFS_UPSTREAM_PRODUCTS = $(foreach package,$(UPSTREAM_PACKAGES) ncurses,$(UPSTREAM_ROOT)/$(package)/root/.complete $(upstream_$(package)_products))
ROOTFS_APP_PRODUCTS = $(USERLAND_DIR)/motd.elf $(addprefix $(USERLAND_DIR)/,$(addsuffix .elf,$(RELIEFOS_COMPONENT_APPS))) $(USERLAND_DIR)/dynlinkerror.elf $(BUSYBOX_ELF) $(BUSYBOX_LINKS) $(SQLITE_SO) $(PORTABLEGL_SO) $(USERLAND_DIR)/sl.elf
ROOTFS_APP_PRODUCTS += $(if $(filter settings soundctl,$(RELIEFOS_COMPONENT_APPS)),$(AUDIO_CONTROL_SO))
$(ROOTFS_RAW_STAMP) $(ROOTFS_MANIFEST): $(NLS_MO) $(NLS_MUSL_MO) $(RELIEFOS_SRC)/configs/nls/LINGUAS
RELIEFOS_SIG_rootfs := epoch=$(SOURCE_DATE_EPOCH)|sources=$(O_META)/rootfs-sources.sig|components=$(RELIEFOS_COMPONENTS_ENABLED)
$(if $(RELIEFOS_PASSIVE),,$(eval $(call RELIEFOS_SIGNATURE_RULE,rootfs)))
$(ROOTFS_RAW_STAMP) $(ROOTFS_MANIFEST) &: $(RELIEFOS_SRC)/tools/build/rootfs-stage.sh $(RELIEFOS_STAGE_TOOL) $(RELIEFOS_LAYOUT_TOOL) $(O_META)/rootfs-sources.sig $(ROOTFS_UPSTREAM_PRODUCTS) $(ROOTFS_APP_PRODUCTS) $(RUNTIME_SO) $(RUNTIME_COMPAT_SO) $(KERNELDEBUG_SYS) $(DRIVER_OUTPUTS) $(COMPONENT_METADATA) $(UI_METRO_FONT) $(UI_WIN95_FONT) $(RELIEFOS_CONFIG_FILE) $(O_META)/rootfs.sig $(wildcard $(RELIEFOS_SRC)/system/xorg/*)
	$(Q)sh $(RELIEFOS_SRC)/tools/build/rootfs-stage.sh $(RELIEFOS_SRC) $(abspath $(O)) $(abspath $(RELIEFOS_CONFIG_FILE)) $(abspath $(COMPONENT_METADATA)) $(abspath $(RELIEFOS_STAGE_TOOL)) $(abspath $(RELIEFOS_LAYOUT_TOOL)) $(abspath $(ROOTFS_RAW)) $(abspath $(ROOTFS_MANIFEST)) $(SOURCE_DATE_EPOCH)
	$(Q)touch $(ROOTFS_RAW_STAMP)
.PHONY: rootfs-raw
rootfs-raw: $(ROOTFS_RAW_STAMP) $(ROOTFS_MANIFEST)
tools: $(RELIEFOS_STAGE_TOOL) $(RELIEFOS_LAYOUT_TOOL)
$(O_META)/rootfs-present.sig: FORCE $(RELIEFOS_STAGE_TOOL)
	$(Q)mkdir -p $(@D)
	$(Q)if ! $(RELIEFOS_STAGE_TOOL) --check $(ROOTFS_MANIFEST) $(ROOTFS_RAW); then touch $@; elif test ! -f $@; then touch $@; fi
$(ROOTFS_RAW_STAMP) $(ROOTFS_MANIFEST): $(O_META)/rootfs-present.sig
