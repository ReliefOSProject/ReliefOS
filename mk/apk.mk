# Signed APK repository and managed root. The rootfs fragment publishes a raw,
# unpackaged tree here; this fragment never infers or rebuilds that producer.
APK_RAW_ROOT ?= $(O_STAGE)/rootfs-raw
APK_RAW_STAMP ?= $(APK_RAW_ROOT)/.complete
APK_MANAGED_ROOT := $(O)/rootfs/managed
APK_WORK := $(O_PACKAGES)/apk
APK_REPOSITORY := $(APK_WORK)/repository
APK_MANIFEST := $(APK_WORK)/manifest.json
APK_OWNERSHIP := $(APK_WORK)/ownership.tsv
APK_SIGNING_KEY ?= $(HOME)/.local/share/leonos/apk-signing/key.pem
# Generation 2 supersedes the generation-1 BusyBox/binutils ownership regression,
# including targets partially updated by those media. Published releases must use
# an increasing SOURCE_DATE_EPOCH (or an explicit increasing version prefix).
APK_BUILD_VERSION ?= 2.$(SOURCE_DATE_EPOCH)-r0
APK_STAGE_SCRIPT := $(RELIEFOS_SRC)/tools/build/apk-stage.sh
APK_STAGE_INPUTS := $(RELIEFOS_SRC)/tools/build/apk-layout.sh $(RELIEFOS_LAYOUT_TOOL) $(RELIEFOS_LOCK) $(RELIEFOS_DEPS_TOOL)
BUILD_LOG_INPUTS := $(RELIEFOS_SHELL_LOG) $(RELIEFOS_SRC)/tools/build/run-logged.sh $(RELIEFOS_SRC)/tools/build/format-log.awk
APK_STAGE_INPUTS += $(BUILD_LOG_INPUTS)

RELIEFOS_SIG_apk-stage := version=$(APK_BUILD_VERSION)|epoch=$(SOURCE_DATE_EPOCH)|raw=$(abspath $(APK_RAW_ROOT))|key=$(APK_SIGNING_KEY)|key-identity=$(shell sha256sum '$(APK_SIGNING_KEY)' 2>/dev/null | cut -d' ' -f1)|policy=$(shell sha256sum $(RELIEFOS_SRC)/configs/apk-ownership.json 2>/dev/null | cut -d' ' -f1)|script=$(shell sha256sum $(APK_STAGE_SCRIPT) 2>/dev/null | cut -d' ' -f1)
$(if $(RELIEFOS_PASSIVE),,$(eval $(call RELIEFOS_SIGNATURE_RULE,apk-stage)))

# rootfs-raw/.complete is a durable producer interface. Its recipe is owned by
# mk/rootfs.mk; keeping it as a prerequisite catches missing staging explicitly.
$(APK_MANAGED_ROOT)/.apk-complete $(APK_REPOSITORY)/packages.adb $(APK_MANIFEST) $(APK_OWNERSHIP) &: \
	$(APK_RAW_STAMP) $(APK_STAGE_SCRIPT) $(APK_STAGE_INPUTS) $(RELIEFOS_APK_OWN) $(UPSTREAM_APK) \
	$(UPSTREAM_APK_ROOT)/.complete $(RELIEFOS_SRC)/configs/apk-ownership.json \
	$(RELIEFOS_SRC)/resources/licenses/apk-tools-LICENSE $(MUSL_STAMP) \
	$(RELIEFOS_SRC)/userland/storage/leonos-apk-update \
	$(RELIEFOS_SRC)/userland/storage/busybox-binutils-links \
	$(RELIEFOS_SRC)/system/rootfs/etc/apk/protected_paths.d/leonos.list \
	$(RELIEFOS_SRC)/system/rootfs/etc/apk/protected_paths.d/reliefos.list \
	$(O_META)/apk-stage.sig
	$(Q)mkdir -p $(O_LOGS) $(O_PACKAGES)
		$(Q)SOURCE_DATE_EPOCH='$(SOURCE_DATE_EPOCH)' APK_MUSL_SYSROOT='$(abspath $(MUSL_SYSROOT))' sh $(RELIEFOS_SRC)/tools/build/run-logged.sh $(O_LOGS)/apk-stage.log sh $(APK_STAGE_SCRIPT) \
			$(RELIEFOS_SRC) $(abspath $(APK_RAW_ROOT)) $(abspath $(APK_MANAGED_ROOT)) \
			$(abspath $(APK_WORK)) $(abspath $(UPSTREAM_APK)) $(abspath $(UPSTREAM_APK_ROOT)) \
			$(abspath $(RELIEFOS_DEPS_TOOL)) $(RELIEFOS_LOCK) \
			$(RELIEFOS_SRC)/configs/apk-ownership.json $(abspath $(RELIEFOS_APK_OWN)) \
			$(APK_SIGNING_KEY) '$(APK_BUILD_VERSION)'
	$(Q)touch $(APK_MANAGED_ROOT)/.apk-complete
	$(Q)sh $(RELIEFOS_SRC)/tools/build/stage-inventory.sh write $(APK_MANAGED_ROOT) $(O_META)/apk-root.files
	$(Q)sh $(RELIEFOS_SRC)/tools/build/stage-inventory.sh write $(APK_REPOSITORY) $(O_META)/apk-repo.files

$(O_META)/apk-root-present.sig: FORCE
	$(Q)sh $(RELIEFOS_SRC)/tools/build/stage-inventory.sh check $(APK_MANAGED_ROOT) $(O_META)/apk-root.files $@
$(O_META)/apk-repo-present.sig: FORCE
	$(Q)sh $(RELIEFOS_SRC)/tools/build/stage-inventory.sh check $(APK_REPOSITORY) $(O_META)/apk-repo.files $@
$(APK_MANAGED_ROOT)/.apk-complete $(APK_REPOSITORY)/packages.adb $(APK_MANIFEST) $(APK_OWNERSHIP): $(O_META)/apk-root-present.sig $(O_META)/apk-repo-present.sig $(RELIEFOS_SRC)/tools/build/stage-inventory.sh

apk-repo: $(APK_MANAGED_ROOT)/.apk-complete $(APK_REPOSITORY)/packages.adb $(APK_MANIFEST) $(APK_OWNERSHIP)
rootfs: $(APK_MANAGED_ROOT)/.apk-complete

.PHONY: test-apk
test-apk:
	$(Q)sh $(RELIEFOS_SRC)/tests/build/test-apk-ownership.sh $(RELIEFOS_SRC)
