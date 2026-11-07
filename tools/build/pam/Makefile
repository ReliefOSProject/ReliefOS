# Linux-PAM 1.7.3, ReliefOS musl feature policy. Source is a patched, read-only tree.
# Parent owns download, patch application, output lock and staging publication.
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
SHELL := /bin/sh
SELF := $(abspath $(lastword $(MAKEFILE_LIST)))
PORT := $(dir $(SELF))
ifeq ($(origin CC),default)
CC := clang
endif
SRC ?=
BUILD ?=
DESTDIR ?=
SYSROOT ?=
DEPS ?=
KERNEL_HEADERS ?= $(DEPS)/usr/include
TARGET ?= x86_64-linux-musl
CFLAGS ?= -O2 -mno-avx -mno-avx2
$(foreach required,SRC BUILD SYSROOT DEPS,$(if $(strip $($(required))),,$(error $(required) is required)))
RESOURCE := $(shell $(CC) -print-resource-dir)
CROSS = --target=$(TARGET) --sysroot=$(SYSROOT) --gcc-toolchain=/nonexistent -fuse-ld=lld --rtlib=compiler-rt --unwindlib=none
INCLUDES = -nostdinc -isystem $(SYSROOT)/include -isystem $(RESOURCE)/include -idirafter $(KERNEL_HEADERS) -I$(BUILD)/include -I$(SRC)/libpam/include -I$(SRC)/libpam_internal/include -I$(SRC)/libpamc/include -I$(SRC)/libpam_misc/include
COMPILE = $(CC) $(filter-out -fuse-ld=lld --rtlib=compiler-rt --unwindlib=none,$(CROSS)) $(INCLUDES) $(CPPFLAGS) $(CFLAGS) -fPIC -ffile-prefix-map=$(SRC)=Linux-PAM-1.7.3 -ffile-prefix-map=$(BUILD)=pam-build
LINK = $(CC) $(CROSS) $(CFLAGS) $(LDFLAGS) -Wl,-z,relro,-z,now,--no-undefined -L$(BUILD)/lib -L$(DEPS)/lib -Wl,-rpath-link,$(DEPS)/lib
CONFIG := $(BUILD)/include/config.h
SIGNATURE := $(BUILD)/arguments
.PHONY: all install FORCE
FORCE:
$(SIGNATURE): FORCE
	@mkdir -p $(@D)
	@set -eu; { printf '%s\n' '$(COMPILE)' '$(LINK)' '$(SRC)'; $(CC) --version; $(BISON) --version; $(FLEX) --version; sha256sum "$$(command -v $(CC))" "$$($(CC) -print-prog-name=ld.lld)" "$$(command -v $(BISON))" "$$(command -v $(FLEX))"; sha256sum '$(SELF)' '$(PORT)configure.sh'; } > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@
$(CONFIG): $(BUILD)/include/crypt.h $(SIGNATURE) $(PORT)configure.sh $(SYSROOT)/lib/libc.so $(DEPS)/lib/libcrypt.so.2
	@CC='$(CC) $(CROSS)' CPPFLAGS='$(INCLUDES) $(CPPFLAGS)' CFLAGS='$(CFLAGS)' LDFLAGS='-L$(DEPS)/lib -Wl,-rpath-link,$(DEPS)/lib' sh $(PORT)configure.sh $@

internal_src := pam_debug pam_econf pam_line
pam_src := pam_account pam_audit pam_auth pam_data pam_delay pam_dispatch pam_dynamic pam_end pam_env pam_get_authtok pam_handlers pam_item pam_misc pam_modutil_check_user pam_modutil_cleanup pam_modutil_getgrgid pam_modutil_getgrnam pam_modutil_getlogin pam_modutil_getpwnam pam_modutil_getpwuid pam_modutil_getspnam pam_modutil_ingroup pam_modutil_ioloop pam_modutil_priv pam_modutil_sanitize pam_modutil_searchkey pam_password pam_prelude pam_session pam_start pam_strerror pam_syslog pam_vprompt
pam_misc_src := help_env misc_conv
pamc_src := pamc_client pamc_converse pamc_load
internal_obj := $(addprefix $(BUILD)/obj/libpam_internal/,$(addsuffix .o,$(internal_src)))
$(BUILD)/obj/%.o: $(SRC)/%.c $(CONFIG) $(SIGNATURE)
	@mkdir -p $(@D)
	$(COMPILE) $(EXTRA) -MD -MP -MF $@.d -MT $@ -c $< -o $@.tmp
	@mv $@.tmp $@
$(addprefix $(BUILD)/obj/libpam/,$(addsuffix .o,$(pam_src))): EXTRA = -DLIBPAM_COMPILE -DDEFAULT_MODULE_PATH='"/lib/security/"'

# Internal objects are linked directly, avoiding stale archive members.
define library
$(BUILD)/lib/lib$(1).so.$(2): $(addprefix $(BUILD)/obj/lib$(1)/,$(addsuffix .o,$($(1)_src))) $(internal_obj) $(SRC)/lib$(1)/lib$(1).map $(if $(3),$(BUILD)/lib/libpam.so.0.85.1)
	@mkdir -p $$(@D)
	$(LINK) -shared -Wl,-soname,lib$(1).so.0 -Wl,--version-script=$(SRC)/lib$(1)/lib$(1).map $$(filter %.o,$$^) $(3) -o $$@.tmp
	@mv $$@.tmp $$@
$(BUILD)/lib/lib$(1).so.0: $(BUILD)/lib/lib$(1).so.$(2)
	ln -sfn lib$(1).so.$(2) $$@
$(BUILD)/lib/lib$(1).so: $(BUILD)/lib/lib$(1).so.0
	ln -sfn lib$(1).so.0 $$@
libraries += $(BUILD)/lib/lib$(1).so $(BUILD)/lib/lib$(1).so.0 $(BUILD)/lib/lib$(1).so.$(2)
endef
$(eval $(call library,pam,0.85.1,))
$(eval $(call library,pamc,0.82.1,))
$(eval $(call library,pam_misc,0.82.1,$(BUILD)/lib/libpam.so.0.85.1))

modules := access canonicalize_user debug deny echo env exec faildelay faillock filter ftp group issue keyinit limits listfile localuser loginuid mail mkhomedir motd namespace nologin permit pwhistory rootok securetty setquota shells stress succeed_if time timestamp umask unix usertype warn wheel xauth
faillock_extra := faillock faillock_config
namespace_extra := md5 argv_parse
pwhistory_extra := opasswd pwhistory_config
timestamp_extra := hmacsha1 sha1
unix_sources := bigcrypt pam_unix_acct pam_unix_auth pam_unix_passwd pam_unix_sess support passverify md5_good md5_broken
limits_flags := -DLIMITS_FILE_DIR='"/etc/security/limits.d"'
mkhomedir_flags := -DMKHOMEDIR_HELPER='"/sbin/mkhomedir_helper"'
unix_flags := -DCHKPWD_HELPER='"/sbin/unix_chkpwd"' -DUPDATE_HELPER='"/sbin/unix_update"'
unix_libs := -lcrypt
pwhistory_libs := -lcrypt
define module
$(1)_objects := $(addprefix $(BUILD)/obj/modules/pam_$(1)/,$(addsuffix .o,$(if $($(1)_sources),$($(1)_sources),pam_$(1) $($(1)_extra))))
$$($(1)_objects): EXTRA = $($(1)_flags)
$(BUILD)/modules/pam_$(1).so: $$($(1)_objects) $(internal_obj) $(BUILD)/lib/libpam.so.0.85.1 $(SRC)/modules/pam_$(1)/module.map $(if $($(1)_libs),$(DEPS)/lib/libcrypt.so.2)
	@mkdir -p $$(@D)
	$(LINK) -shared -Wl,--version-script=$(SRC)/modules/pam_$(1)/module.map $$(filter %.o,$$^) $(BUILD)/lib/libpam.so.0.85.1 $($(1)_libs) -o $$@.tmp
	@mv $$@.tmp $$@
endef
$(foreach m,$(modules),$(eval $(call module,$(m))))

# Helpers use distinct object namespaces because HELPER_COMPILE changes ABI paths.
define helper
$(BUILD)/obj/helpers/$(1)/%.o: $(SRC)/modules/pam_$(2)/%.c $(CONFIG) $(SIGNATURE)
	@mkdir -p $$(@D)
	$(COMPILE) -I$(SRC)/modules/pam_$(2) -DHELPER_COMPILE='"$(1)"' -MD -MP -MF $$@.d -MT $$@ -c $$< -o $$@.tmp
	@mv $$@.tmp $$@
$(BUILD)/bin/$(1): $(addprefix $(BUILD)/obj/helpers/$(1)/,$(addsuffix .o,$(3))) $(internal_obj) $(BUILD)/lib/libpam.so.0.85.1 $(if $(4),$(DEPS)/lib/libcrypt.so.2)
	@mkdir -p $$(@D)
	$(LINK) -pie $$(filter %.o,$$^) $(BUILD)/lib/libpam.so.0.85.1 $(4) -o $$@.tmp
	@mv $$@.tmp $$@
helpers += $(BUILD)/bin/$(1)
endef
$(eval $(call helper,faillock,faillock,main faillock faillock_config,))
$(eval $(call helper,mkhomedir_helper,mkhomedir,mkhomedir_helper,))
$(eval $(call helper,pwhistory_helper,pwhistory,pwhistory_helper opasswd,-lcrypt))
$(eval $(call helper,pam_timestamp_check,timestamp,pam_timestamp_check,))
$(eval $(call helper,unix_chkpwd,unix,unix_chkpwd audit bigcrypt md5_good md5_broken passverify,-lcrypt))
$(eval $(call helper,upperLOWER,filter,upperLOWER/upperLOWER,))

all: $(libraries) $(addprefix $(BUILD)/modules/pam_,$(addsuffix .so,$(modules))) $(helpers)
install: all
	@test -n '$(DESTDIR)' || { echo 'DESTDIR required' >&2; exit 2; }
	@sh $(PORT)install.sh '$(SRC)' '$(BUILD)' '$(DESTDIR)'
-include $(shell test ! -d '$(BUILD)/obj' || find '$(BUILD)/obj' -name '*.o.d')

# Upstream also builds this non-installed diagnostic tool.
$(eval $(call helper,hmacfile,timestamp,hmacfile hmacsha1 sha1,))
BISON ?= bison
FLEX ?= flex
$(BUILD)/generated/pam_conv_y.c $(BUILD)/generated/pam_conv_y.h &: $(SRC)/conf/pam_conv1/pam_conv_y.y $(SIGNATURE)
	@mkdir -p $(@D)
	@set -eu; tmp=$$(mktemp -d '$(@D)/.yacc.XXXXXX'); trap 'rm -rf "$$tmp"' EXIT HUP INT TERM; \
	$(BISON) --no-lines --file-prefix-map="$$tmp"=generated -o "$$tmp/pam_conv_y.c" --defines="$$tmp/pam_conv_y.h" $<; \
	mv "$$tmp/pam_conv_y.c" $(BUILD)/generated/pam_conv_y.c; \
	mv "$$tmp/pam_conv_y.h" $(BUILD)/generated/pam_conv_y.h
$(BUILD)/generated/pam_conv_l.c: $(SRC)/conf/pam_conv1/pam_conv_l.l $(BUILD)/generated/pam_conv_y.h $(SIGNATURE)
	$(FLEX) --noline -o $@.tmp $<
	@mv $@.tmp $@
$(BUILD)/obj/generated/%.o: $(BUILD)/generated/%.c $(BUILD)/generated/pam_conv_y.h $(CONFIG) $(SIGNATURE)
	@mkdir -p $(@D)
	$(COMPILE) -I$(BUILD)/generated -MD -MP -MF $@.d -MT $@ -c $< -o $@.tmp
	@mv $@.tmp $@
$(BUILD)/bin/pam_conv1: $(BUILD)/obj/generated/pam_conv_l.o $(BUILD)/obj/generated/pam_conv_y.o
	@mkdir -p $(@D)
	$(LINK) -pie $^ -o $@.tmp
	@mv $@.tmp $@
all: $(BUILD)/bin/pam_conv1 $(BUILD)/bin/hmacfile

# Keep UAPI behind musl while selecting libxcrypt's extended API declaration.
# Passing the same directory to both -I and -idirafter makes Clang demote -I.
$(BUILD)/include/crypt.h: $(DEPS)/usr/include/crypt.h
	@mkdir -p $(@D)
	@cp $< $@.tmp
	@mv $@.tmp $@
