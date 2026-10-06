# Include after third-party.mk, pam.mk and runtime.mk. Each package owns an isolated root.
UPSTREAM_ROOT := $(O)/upstream
UPSTREAM_SCRIPT := $(RELIEFOS_SRC)/tools/build/upstream.sh
UPSTREAM_PACKAGES := libmd libbsd util-linux sudo shadow e2fsprogs dosfstools exfatprogs coreutils alsa-lib alsa-utils nuked-opl3
upstream_libmd_outputs := lib/libmd.so.0 usr/include/md5.h
upstream_libbsd_outputs := lib/libbsd.so.0 usr/include/bsd/stdlib.h
upstream_util-linux_outputs := usr/lib/libuuid.a usr/lib/libblkid.a bin/su usr/sbin/fdisk bin/mount bin/lsblk
upstream_sudo_outputs := usr/bin/sudo usr/lib/sudo/sudoers.so
upstream_shadow_outputs := bin/login usr/sbin/useradd usr/sbin/usermod usr/sbin/userdel
upstream_e2fsprogs_outputs := usr/sbin/mkfs.ext4 usr/sbin/fsck.ext4 usr/sbin/mkfs.ext2 usr/sbin/fsck.ext2
upstream_dosfstools_outputs := usr/sbin/mkfs.fat usr/sbin/fsck.fat
upstream_exfatprogs_outputs := usr/sbin/mkfs.exfat usr/sbin/fsck.exfat
upstream_coreutils_outputs := usr/bin/dd
upstream_ncurses_outputs := usr/lib/libncursesw.a usr/lib/libtinfow.a usr/include/curses.h
upstream_alsa-lib_outputs := usr/lib/libasound.so.2 usr/lib/libasound.a usr/include/alsa/asoundlib.h usr/share/alsa/alsa.conf
upstream_alsa-utils_outputs := usr/bin/aplay usr/bin/arecord usr/bin/amixer usr/sbin/alsactl usr/bin/speaker-test
upstream_nuked-opl3_outputs := usr/lib/libopl3.so.1 usr/include/opl3.h
upstream_nuked-opl3_script := $(RELIEFOS_SRC)/tools/build/opl3.sh
$(foreach package,$(UPSTREAM_PACKAGES) ncurses,$(eval upstream_$(package)_primary := $(upstream_$(package)_outputs)))
upstream_libbsd_deps := libmd
upstream_shadow_deps := libmd libbsd
upstream_e2fsprogs_deps := util-linux
upstream_exfatprogs_deps := util-linux
upstream_alsa-utils_deps := alsa-lib
upstream_alsa-lib_patches := $(wildcard $(RELIEFOS_SRC)/patches/alsa-lib/*.patch)
# Install manifests register every published file, so deleting a non-primary
# header, library link or manual page also invalidates the package group.

define RELIEFOS_UPSTREAM_RULE
upstream_$(1)_script ?= $$(UPSTREAM_SCRIPT)
RELIEFOS_SIG_upstream-$(1) := cc=$(TARGET_CC)|triple=$(TRIPLE_USER)|lock=$(RELIEFOS_LOCK_DIGEST)|identity=$(shell $(TARGET_CC) --version 2>/dev/null | head -n1)
$$(if $$(RELIEFOS_PASSIVE),,$$(eval $$(call RELIEFOS_SIGNATURE_RULE,upstream-$(1))))
upstream_$(1)_products := $$(addprefix $$(UPSTREAM_ROOT)/$(1)/root/,$$(sort $$(upstream_$(1)_outputs)))
$$(UPSTREAM_ROOT)/$(1)/root/.complete $$(upstream_$(1)_products) &: $$(upstream_$(1)_script) $$(upstream_$(1)_patches) $$(RELIEFOS_SRC)/mk/upstream.mk $$(RELIEFOS_LOCK) $$(RELIEFOS_DEPS_TOOL) $$(O_META)/upstream-$(1).sig $$(AUTH_STAMP) $$(RELIEFOS_AUTH_ARTIFACTS) $$(PAM_STAMP) $$(PAM_LIB) $$(PAM_HEADER) $$(foreach dep,$$(upstream_$(1)_deps),$$(UPSTREAM_ROOT)/$$(dep)/root/.complete $$(upstream_$$(dep)_products))
	$$(Q)rm -rf $$(UPSTREAM_ROOT)/$(1)/deps
	$$(Q)mkdir -p $$(UPSTREAM_ROOT)/$(1)/deps $$(O_LOGS)
	$$(Q)cp -a $$(AUTH_ROOT)/. $$(UPSTREAM_ROOT)/$(1)/deps/
	$$(Q)$$(foreach dep,$$(upstream_$(1)_deps),cp -a $$(UPSTREAM_ROOT)/$$(dep)/root/. $$(UPSTREAM_ROOT)/$(1)/deps/;)
	+$$(Q)case "$$$${MAKEFLAGS%% *}" in *n*) exit 0;; esac; sh $$(upstream_$(1)_script) $(1) $$(RELIEFOS_SRC) $$(abspath $$(RELIEFOS_DEPS_TOOL)) $$(RELIEFOS_LOCK) $$(RELIEFOS_CACHE) $$(abspath $$(UPSTREAM_ROOT))/$(1)/work $$(abspath $$(UPSTREAM_ROOT))/$(1)/root $$(abspath $$(MUSL_SYSROOT)) $$(abspath $$(UPSTREAM_ROOT))/$(1)/deps $$(abspath $$(PAM_ROOT)) $$(TARGET_CC) $$(TRIPLE_USER) >$$(O_LOGS)/upstream-$(1).log 2>&1 || { tail -n 50 $$(O_LOGS)/upstream-$(1).log >&2; exit 1; }
	$$(Q)for product in $$(addprefix $$(UPSTREAM_ROOT)/$(1)/root/,$$(upstream_$(1)_primary)); do test -e "$$$$product" || { echo "missing upstream product: $$$$product" >&2; exit 1; }; touch "$$$$product"; done
	$$(Q)touch $$(UPSTREAM_ROOT)/$(1)/root/.complete
.PHONY: upstream-$(1)
upstream-$(1): $$(UPSTREAM_ROOT)/$(1)/root/.complete $$(upstream_$(1)_products)
endef
$(foreach package,$(UPSTREAM_PACKAGES),$(eval $(call RELIEFOS_UPSTREAM_RULE,$(package))))
.PHONY: reliefos-upstream reliefos-storage leonos-upstream leonos-storage
reliefos-upstream: $(addprefix upstream-,$(UPSTREAM_PACKAGES))
reliefos-storage: upstream-e2fsprogs upstream-dosfstools upstream-exfatprogs
leonos-upstream: reliefos-upstream
leonos-storage: reliefos-storage

# Terminal packages use the pinned submodules and upstream Makefiles.
UPSTREAM_TERMINAL_SCRIPT := $(RELIEFOS_SRC)/tools/build/upstream-terminal.sh
define RELIEFOS_TERMINAL_RULE
RELIEFOS_SIG_upstream-$(1) := cc=$(TARGET_CC)|triple=$(TRIPLE_USER)|lock=$(RELIEFOS_LOCK_DIGEST)|identity=$(shell $(TARGET_CC) --version 2>/dev/null | head -n1)
$$(if $$(RELIEFOS_PASSIVE),,$$(eval $$(call RELIEFOS_SIGNATURE_RULE,upstream-$(1))))
upstream_$(1)_products := $$(addprefix $$(UPSTREAM_ROOT)/$(1)/root/,$$(sort $$(upstream_$(1)_outputs)))
$$(UPSTREAM_ROOT)/$(1)/root/.complete $$(upstream_$(1)_products) &: $$(UPSTREAM_TERMINAL_SCRIPT) $$(RELIEFOS_LOCK) $$(RELIEFOS_DEPS_TOOL) $$(O_META)/upstream-$(1).sig $$(MUSL_STAMP) $$(RELIEFOS_MUSL_ARTIFACTS)
	$$(Q)mkdir -p $$(O_LOGS)
	+$$(Q)case "$$$${MAKEFLAGS%% *}" in *n*) exit 0;; esac; sh $$(UPSTREAM_TERMINAL_SCRIPT) $(1) $$(RELIEFOS_SRC) $$(abspath $$(RELIEFOS_DEPS_TOOL)) $$(RELIEFOS_LOCK) $$(abspath $$(UPSTREAM_ROOT))/$(1)/work $$(abspath $$(UPSTREAM_ROOT))/$(1)/root $$(abspath $$(MUSL_SYSROOT)) $$(TARGET_CC) $$(TRIPLE_USER) >$$(O_LOGS)/upstream-$(1).log 2>&1 || { tail -n 50 $$(O_LOGS)/upstream-$(1).log >&2; exit 1; }
	$$(Q)for product in $$(addprefix $$(UPSTREAM_ROOT)/$(1)/root/,$$(upstream_$(1)_primary)); do test -e "$$$$product" || exit 1; touch "$$$$product"; done
	$$(Q)touch $$(UPSTREAM_ROOT)/$(1)/root/.complete
.PHONY: upstream-$(1)
upstream-$(1): $$(UPSTREAM_ROOT)/$(1)/root/.complete $$(upstream_$(1)_products)
endef
$(foreach package,ncurses,$(eval $(call RELIEFOS_TERMINAL_RULE,$(package))))
reliefos-upstream: upstream-ncurses

UPSTREAM_APK_ROOT := $(UPSTREAM_ROOT)/apk
UPSTREAM_APK := $(UPSTREAM_APK_ROOT)/apk.static
$(UPSTREAM_APK) $(UPSTREAM_APK_ROOT)/.complete &: $(RELIEFOS_SRC)/tools/build/upstream-apk.sh $(RELIEFOS_LOCK) $(RELIEFOS_DEPS_TOOL) $(wildcard $(RELIEFOS_SRC)/system/rootfs/etc/apk/keys/*)
	$(Q)sh $(RELIEFOS_SRC)/tools/build/upstream-apk.sh $(RELIEFOS_SRC) $(abspath $(RELIEFOS_DEPS_TOOL)) $(RELIEFOS_LOCK) $(RELIEFOS_CACHE) $(abspath $(UPSTREAM_APK_ROOT))
.PHONY: upstream-apk
upstream-apk: $(UPSTREAM_APK) $(UPSTREAM_APK_ROOT)/.complete
reliefos-upstream: upstream-apk

UPSTREAM_APP_DIR := $(O)/userland
SQLITE_SO := $(UPSTREAM_APP_DIR)/sqlite.so.3
SQLITE_HEADER := $(UPSTREAM_APP_DIR)/sqlite3.h
upstream_app_sl_outputs := sl.elf
upstream_app_sqlite_outputs := sqlite.so.3 libsqlite3.a sqlite3.h

define RELIEFOS_UPSTREAM_APP
upstream_app_$(1)_inputs := $$(shell find $$(RELIEFOS_SRC)/third_party/$(if $(filter pleditor,$(1)),pl_editor,$(1)) $$(RELIEFOS_SRC)/userland/$(if $(filter pleditor,$(1)),apps/pleditor,$(1)) -type f ! -name .git 2>/dev/null | LC_ALL=C sort)
RELIEFOS_SIG_upstream-app-$(1) := cc=$(TARGET_CC)|identity=$(shell $(TARGET_CC) --version 2>/dev/null | head -n1)|ld=$(TARGET_LD)|ar=$(TARGET_AR)|cflags=$(RELIEFOS_OPTIMIZATION_FLAGS)|ldflags=$(RELIEFOS_LINK_POLICY_FLAGS)|inputs=$$(upstream_app_$(1)_inputs)
$$(if $$(RELIEFOS_PASSIVE),,$$(eval $$(call RELIEFOS_SIGNATURE_RULE,upstream-app-$(1))))
$$(addprefix $$(UPSTREAM_APP_DIR)/,$$(sort $$(upstream_app_$(1)_outputs))) &: $$(upstream_app_$(1)_inputs) $$(RELIEFOS_SRC)/tools/build/upstream-app.sh $$(RELIEFOS_LOCK) $$(AUTOCONF_H) $$(RUNTIME_SO) $$(RUNTIME_ARCHIVE) $$(MUSL_STAMP) $$(HEADER_EXPORT_MANIFEST) $$(O_META)/upstream-app-$(1).sig
	$$(Q)mkdir -p $$(O_LOGS)
	+$$(Q)case "$$$${MAKEFLAGS%% *}" in *n*) exit 0;; esac; UPSTREAM_DEPS='$$(abspath $$(RELIEFOS_DEPS_TOOL))' UPSTREAM_UAPI='$$(abspath $$(HEADER_EXPORT_INCLUDE))' UPSTREAM_CFLAGS='$$(RELIEFOS_OPTIMIZATION_FLAGS)' UPSTREAM_LDFLAGS='$$(RELIEFOS_LINK_POLICY_FLAGS)' sh $$(RELIEFOS_SRC)/tools/build/upstream-app.sh $(1) $$(RELIEFOS_SRC) $$(abspath $$(UPSTREAM_ROOT))/app-$(1) $$(abspath $$(UPSTREAM_APP_DIR)) $$(abspath $$(MUSL_SYSROOT)) $$(abspath $$(RUNTIME_SO)) $$(abspath $$(RUNTIME_ARCHIVE)) $$(abspath $$(O_INCLUDE)) $$(abspath $$(AUTOCONF_H)) $$(TARGET_CC) $$(TARGET_LD) $$(TARGET_AR) >$$(O_LOGS)/upstream-app-$(1).log 2>&1 || { tail -n 40 $$(O_LOGS)/upstream-app-$(1).log >&2; exit 1; }
	$$(Q)for product in $$(addprefix $$(UPSTREAM_APP_DIR)/,$$(upstream_app_$(1)_outputs)); do test -e "$$$$product" || exit 1; touch "$$$$product"; done
.PHONY: upstream-app-$(1)
upstream-app-$(1): $$(addprefix $$(UPSTREAM_APP_DIR)/,$$(sort $$(upstream_app_$(1)_outputs)))
endef
$(foreach package,sl sqlite,$(eval $(call RELIEFOS_UPSTREAM_APP,$(package))))
reliefos-upstream: $(addprefix upstream-app-,sl sqlite)

BUSYBOX_ELF := $(UPSTREAM_APP_DIR)/busybox.elf
BUSYBOX_LINKS := $(UPSTREAM_APP_DIR)/busybox.links
UPSTREAM_EPOCH := $(if $(SOURCE_DATE_EPOCH),$(SOURCE_DATE_EPOCH),$(shell git -C $(RELIEFOS_SRC) log -1 --format=%ct))
RELIEFOS_SIG_upstream-busybox := cc=$(TARGET_CC)|identity=$(shell $(TARGET_CC) --version 2>/dev/null | head -n1)|flags=$(RELIEFOS_OPTIMIZATION_FLAGS)|ldflags=$(RELIEFOS_LINK_POLICY_FLAGS)|epoch=$(UPSTREAM_EPOCH)
$(if $(RELIEFOS_PASSIVE),,$(eval $(call RELIEFOS_SIGNATURE_RULE,upstream-busybox)))
$(BUSYBOX_ELF) $(BUSYBOX_LINKS) &: $(RELIEFOS_SRC)/tools/build/upstream-busybox.sh $(RELIEFOS_SRC)/tools/build/busybox-check.sh $(RELIEFOS_SRC)/userland/busybox/leonos.config $(O_META)/upstream-busybox.sig $(RELIEFOS_LOCK) $(RELIEFOS_DEPS_TOOL) $(MUSL_STAMP) $(RELIEFOS_MUSL_ARTIFACTS) $(AUTH_STAMP) $(RELIEFOS_AUTH_ARTIFACTS)
	$(Q)mkdir -p $(O_LOGS)
	+$(Q)case "$${MAKEFLAGS%% *}" in *n*) exit 0;; esac; UPSTREAM_CFLAGS='$(RELIEFOS_OPTIMIZATION_FLAGS)' UPSTREAM_LDFLAGS='$(RELIEFOS_LINK_POLICY_FLAGS)' sh $(RELIEFOS_SRC)/tools/build/upstream-busybox.sh $(RELIEFOS_SRC) $(abspath $(UPSTREAM_ROOT))/busybox $(abspath $(UPSTREAM_APP_DIR)) $(abspath $(MUSL_SYSROOT)) $(abspath $(AUTH_ROOT))/usr/include $(TARGET_CC) $(abspath $(RELIEFOS_DEPS_TOOL)) $(RELIEFOS_LOCK) $(UPSTREAM_EPOCH) >$(O_LOGS)/upstream-busybox.log 2>&1 || { tail -n 50 $(O_LOGS)/upstream-busybox.log >&2; exit 1; }
.PHONY: upstream-busybox
upstream-busybox: $(BUSYBOX_ELF) $(BUSYBOX_LINKS)
reliefos-upstream: upstream-busybox
upstream_app_pleditor_outputs := pleditor.elf
$(eval $(call RELIEFOS_UPSTREAM_APP,pleditor))
$(UPSTREAM_APP_DIR)/pleditor.elf: $(RELIEFOS_SRC)/userland/apps/pleditor/platform_reliefos.c $(RELIEFOS_SRC)/mk/upstream.mk
reliefos-upstream: upstream-app-pleditor

FASTFETCH_ELF := $(UPSTREAM_APP_DIR)/fastfetch.elf
$(FASTFETCH_ELF): $(RELIEFOS_LOCK) $(RELIEFOS_DEPS_TOOL) $(RELIEFOS_SRC)/mk/upstream.mk
	$(Q)mkdir -p $(dir $@)
	$(Q)set -eu; url=$$($(RELIEFOS_DEPS_TOOL) --lock $(RELIEFOS_LOCK) --id fastfetch --print url); digest=$$($(RELIEFOS_DEPS_TOOL) --lock $(RELIEFOS_LOCK) --id fastfetch --print sha256); source=$(RELIEFOS_CACHE)/$${url##*/}; test -f "$$source" || { echo 'missing fastfetch: run make fetch' >&2; exit 1; }; test "$$(sha256sum "$$source" | cut -d' ' -f1)" = "$$digest"; readelf -h "$$source" | grep -q 'Advanced Micro Devices X86-64'; if readelf -l -d "$$source" | grep -E 'INTERP|\(NEEDED\)'; then exit 1; fi; cp "$$source" $@.tmp; chmod 755 $@.tmp; mv $@.tmp $@
reliefos-upstream: $(FASTFETCH_ELF)

# A content/presence signature avoids creating thousands of grouped peer nodes
# for terminfo/manpages, while detecting deletion of every installed product.
define RELIEFOS_UPSTREAM_PRESENCE
$(O_META)/upstream-$(1)-present.sig: FORCE $(RELIEFOS_SRC)/tools/build/upstream-verify.sh
	$(Q)sh $(RELIEFOS_SRC)/tools/build/upstream-verify.sh $(UPSTREAM_ROOT)/$(1)/root $(UPSTREAM_ROOT)/$(1)/work/installed-files $$@
$(UPSTREAM_ROOT)/$(1)/root/.complete $$(upstream_$(1)_products): $(O_META)/upstream-$(1)-present.sig
endef
$(foreach package,$(UPSTREAM_PACKAGES) ncurses,$(eval $(call RELIEFOS_UPSTREAM_PRESENCE,$(package))))
$(O_META)/upstream-apk-present.sig: FORCE $(RELIEFOS_SRC)/tools/build/upstream-verify.sh
	$(Q)sh $(RELIEFOS_SRC)/tools/build/upstream-verify.sh $(UPSTREAM_APK_ROOT) $(UPSTREAM_APK_ROOT)/installed-files $@
$(UPSTREAM_APK) $(UPSTREAM_APK_ROOT)/.complete: $(O_META)/upstream-apk-present.sig
