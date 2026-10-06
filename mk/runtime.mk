# ReliefOS extensions; musl alone owns POSIX and executable startup.
RUNTIME_DIR := $(O)/system/lib
RUNTIME_SO := $(RUNTIME_DIR)/libreliefos.so.2
RUNTIME_COMPAT_SO := $(RUNTIME_DIR)/libleonos.so.2
RUNTIME_ARCHIVE := $(O)/musl/lib/libreliefos.a
RUNTIME_COMPAT_ARCHIVE := $(O)/musl/lib/libleonos.a
RUNTIME_INSTALLER_SO := $(O)/installer/lib/libreliefos.so.2
RUNTIME_INSTALLER_COMPAT_SO := $(O)/installer/lib/libleonos.so.2
RUNTIME_INSTALLER_ARCHIVE := $(O)/musl/lib/libreliefos-installer.a
RUNTIME_INSTALLER_COMPAT_ARCHIVE := $(O)/musl/lib/libleonos-installer.a
GBK_TABLE := $(O_INCLUDE)/generated/reliefos_gbk_table.h
PNG_CONFIG := $(O_INCLUDE)/libpng/pnglibconf.h

RUNTIME_MBEDTLS_NAMES := aes asn1parse asn1write base64 bignum bignum_core bignum_mod \
 bignum_mod_raw block_cipher cipher cipher_wrap constant_time ctr_drbg ecdh ecdsa ecp \
 ecp_curves entropy entropy_poll error gcm md oid pem pk pk_ecc pk_wrap pkcs5 pkparse \
 pkwrite platform platform_util rsa rsa_alt_helpers sha1 sha256 sha512 \
 ssl_ciphersuites ssl_client ssl_msg ssl_tls ssl_tls12_client x509 x509_crt
RUNTIME_ZLIB_NAMES := adler32 compress crc32 deflate infback inffast inflate inftrees trees uncompr zutil
RUNTIME_PNG_NAMES := png pngerror pngget pngmem pngpread pngread pngrio pngrtran pngrutil \
 pngset pngtrans pngwio pngwrite pngwtran pngwutil
RUNTIME_SOURCES := $(sort $(patsubst $(RELIEFOS_SRC)/%,%,$(wildcard \
 $(RELIEFOS_SRC)/userland/runtime/src/*.c $(RELIEFOS_SRC)/userland/runtime/src/*.S \
 $(RELIEFOS_SRC)/userland/auth/*.c)) \
 $(addprefix third_party/mbedtls/library/,$(addsuffix .c,$(RUNTIME_MBEDTLS_NAMES))) \
 $(addprefix third_party/zlib/,$(addsuffix .c,$(RUNTIME_ZLIB_NAMES))) \
 $(addprefix third_party/libpng/,$(addsuffix .c,$(RUNTIME_PNG_NAMES))))
RUNTIME_OBJECTS := $(addprefix $(O_OBJ)/runtime/,$(addsuffix .o,$(RUNTIME_SOURCES)))
RUNTIME_INSTALLER_OBJECTS := $(addprefix $(O_OBJ)/installer-runtime/,$(addsuffix .o,$(RUNTIME_SOURCES)))
RUNTIME_FLAGS := --target=$(TRIPLE_USER) $(RELIEFOS_OPTIMIZATION_FLAGS) -std=c11 \
 -ffreestanding -fno-stack-protector -fPIC -ffunction-sections -fdata-sections \
 -Wall -Wextra -DRELIEFOS_USE_MUSL -D_GNU_SOURCE -mno-avx -mno-avx2 \
 -I$(HEADER_EXPORT_INCLUDE) -I$(PAM_ROOT)/usr/include -I$(AUTH_ROOT)/usr/include -I$(MUSL_SYSROOT)/include \
 -I$(RELIEFOS_SRC)/userland/runtime/include \
 -I$(O_INCLUDE) -I$(RELIEFOS_SRC)/include -I$(RELIEFOS_SRC)/third_party/mbedtls/include \
 -I$(RELIEFOS_SRC)/third_party/zlib -I$(RELIEFOS_SRC)/third_party/libpng \
 -I$(O_INCLUDE)/libpng -DMBEDTLS_CONFIG_FILE='"reliefos_mbedtls_config.h"' \
 -ffile-prefix-map=$(RELIEFOS_SRC)=. -ffile-prefix-map=$(O)=out
RUNTIME_CFLAGS ?=
RUNTIME_AUTH_LIBS := $(PAM_LIB) $(AUTH_ROOT)/lib/libcrypt.so.2
RUNTIME_HEADERS := $(PAM_HEADER) $(AUTH_ROOT)/usr/include/crypt.h
# Resolve the archive from the selected compiler, not from an unrelated LLVM install.
RUNTIME_BUILTINS := $(if $(RELIEFOS_PASSIVE),,$(shell $(TARGET_CC) --target=$(TRIPLE_USER) --rtlib=compiler-rt -print-libgcc-file-name 2>/dev/null))

RELIEFOS_SIG_runtime-cc := cc=$(TARGET_CC)|identity=$(shell $(TARGET_CC) --version 2>/dev/null | head -n1)|flags=$(RUNTIME_FLAGS) $(RUNTIME_CFLAGS)
RELIEFOS_SIG_runtime-link := ld=$(TARGET_LD)|identity=$(shell $(TARGET_LD) --version 2>/dev/null | head -n1)|ar=$(TARGET_AR)|builtins=$(RUNTIME_BUILTINS)|sources=$(RUNTIME_SOURCES)
$(if $(RELIEFOS_PASSIVE),,$(eval $(call RELIEFOS_SIGNATURE_RULE,runtime-cc)))
$(if $(RELIEFOS_PASSIVE),,$(eval $(call RELIEFOS_SIGNATURE_RULE,runtime-link)))

$(GBK_TABLE): $(RELIEFOS_GBK_TOOL) $(RELIEFOS_SRC)/third_party/litehtml/src/encodings.cpp
	$(Q)mkdir -p $(dir $@)
	$(Q)$(RELIEFOS_GBK_TOOL) $(RELIEFOS_SRC)/third_party/litehtml/src/encodings.cpp $@

$(PNG_CONFIG): $(RELIEFOS_SRC)/third_party/libpng/scripts/pnglibconf.h.prebuilt $(RELIEFOS_SRC)/tools/build/png-config.sh $(RELIEFOS_EMIT)
	$(Q)mkdir -p $(dir $@)
	$(Q)sh $(RELIEFOS_SRC)/tools/build/png-config.sh $< $@ $(RELIEFOS_EMIT)

define RELIEFOS_RUNTIME_COMPILE
$(O_OBJ)/$(1)/%.c.o: $(RELIEFOS_SRC)/%.c $(2) $(GBK_TABLE) $(PNG_CONFIG) $(MUSL_STAMP) $(RUNTIME_HEADERS) $(HEADER_EXPORT_MANIFEST) $(O_META)/runtime-cc.sig
	$$(Q)mkdir -p $$(dir $$@)
	$$(call RELIEFOS_LOG,CC,$$<)
	$$(Q)$$(TARGET_CC) $$(RUNTIME_FLAGS) $$(RUNTIME_CFLAGS) -include $(2) \
	 $$(if $$(findstring /zlib/,$$<),-DZ_SOLO -include stddef.h) \
	 $$(if $$(findstring /libpng/,$$<),-DRELIEFOS_LIBPNG_FIXED_POINT=3) \
	 -MMD -MP -MF $$@.d -MT $$@ -c $$< -o $$@.tmp
	$$(Q)mv $$@.tmp $$@
$(O_OBJ)/$(1)/%.S.o: $(RELIEFOS_SRC)/%.S $(2) $(HEADER_EXPORT_MANIFEST) $(O_META)/runtime-cc.sig
	$$(Q)mkdir -p $$(dir $$@)
	$$(Q)$$(TARGET_CC) --target=$$(TRIPLE_USER) -fPIC -I$$(HEADER_EXPORT_INCLUDE) -MMD -MP -MF $$@.d -MT $$@ -c $$< -o $$@.tmp
	$$(Q)mv $$@.tmp $$@
endef
$(eval $(call RELIEFOS_RUNTIME_COMPILE,runtime,$(AUTOCONF_H)))
$(eval $(call RELIEFOS_RUNTIME_COMPILE,installer-runtime,$(AUTOCONF_INSTALLER_H)))

define RELIEFOS_RUNTIME_SHARED_RULE
$(1): $(2) $(RUNTIME_AUTH_LIBS) $(MUSL_SYSROOT)/lib/libc.so $(MUSL_SYSROOT)/lib/libmimalloc.so.3 $(O_META)/runtime-link.sig $(RUNTIME_BUILTINS)
	$$(Q)mkdir -p $$(dir $$@)
	$(Q)test -f '$(RUNTIME_BUILTINS)' || { echo 'missing compiler-rt builtins; select a complete Clang toolchain' >&2; exit 1; }
	$$(Q)$(TARGET_LD) -shared --no-undefined --hash-style=both $(3) -soname $(4) -o $$@.tmp \
	 $(2) -L$(MUSL_SYSROOT)/lib -l:libmimalloc.so.3 $(RUNTIME_AUTH_LIBS) -lc $(RUNTIME_BUILTINS)
	$$(Q)mv $$@.tmp $$@
endef
$(eval $(call RELIEFOS_RUNTIME_SHARED_RULE,$(RUNTIME_SO),$(RUNTIME_OBJECTS),,libreliefos.so.2))
$(eval $(call RELIEFOS_RUNTIME_SHARED_RULE,$(RUNTIME_COMPAT_SO),$(RUNTIME_OBJECTS),,libleonos.so.2))
$(eval $(call RELIEFOS_RUNTIME_SHARED_RULE,$(RUNTIME_INSTALLER_SO),$(RUNTIME_INSTALLER_OBJECTS),-z max-page-size=0x1000,libreliefos.so.2))
$(eval $(call RELIEFOS_RUNTIME_SHARED_RULE,$(RUNTIME_INSTALLER_COMPAT_SO),$(RUNTIME_INSTALLER_OBJECTS),-z max-page-size=0x1000,libleonos.so.2))

define RELIEFOS_RUNTIME_ARCHIVE_RULE
$(1): $(2) $(O_META)/runtime-link.sig
	$$(Q)mkdir -p $$(dir $$@)
	$$(Q)rm -f $$@.tmp
	$$(Q)$(TARGET_AR) rcsD $$@.tmp $(2)
	$$(Q)mv $$@.tmp $$@
endef
$(eval $(call RELIEFOS_RUNTIME_ARCHIVE_RULE,$(RUNTIME_ARCHIVE),$(RUNTIME_OBJECTS)))
$(eval $(call RELIEFOS_RUNTIME_ARCHIVE_RULE,$(RUNTIME_COMPAT_ARCHIVE),$(RUNTIME_OBJECTS)))
$(eval $(call RELIEFOS_RUNTIME_ARCHIVE_RULE,$(RUNTIME_INSTALLER_ARCHIVE),$(RUNTIME_INSTALLER_OBJECTS)))
$(eval $(call RELIEFOS_RUNTIME_ARCHIVE_RULE,$(RUNTIME_INSTALLER_COMPAT_ARCHIVE),$(RUNTIME_INSTALLER_OBJECTS)))

runtime: $(RUNTIME_SO) $(RUNTIME_COMPAT_SO) $(RUNTIME_ARCHIVE) $(RUNTIME_COMPAT_ARCHIVE) \
	$(RUNTIME_INSTALLER_SO) $(RUNTIME_INSTALLER_COMPAT_SO) \
	$(RUNTIME_INSTALLER_ARCHIVE) $(RUNTIME_INSTALLER_COMPAT_ARCHIVE)
-include $(addsuffix .d,$(RUNTIME_OBJECTS) $(RUNTIME_INSTALLER_OBJECTS))
