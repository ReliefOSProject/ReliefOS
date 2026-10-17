# Locale / NLS pipeline.
#
# The tool variable and its RELIEFOS_HOST_TOOLS registration live in mk/host.mk;
# only the link rule is here, because the object comes from that file's generic
# host rule, which already matches tools/host/nls/*.c.
$(RELIEFOS_NLS_EXTRACT): $(O_HOST)/obj/tools/host/nls/reliefos-nls-extract.c.o | $(RELIEFOS_HOST_BIN)
	$(call RELIEFOS_LOG,HOSTLD,$@)
	$(Q)$(HOSTCC) $(HOST_CFLAGS) $(HOST_LDFLAGS) $^ -o $@

RELIEFOS_NLS_PO := $(RELIEFOS_SRC)/configs/nls/po
RELIEFOS_NLS_LANGS := $(strip $(shell cat $(RELIEFOS_SRC)/configs/nls/LINGUAS))
NLS_DIR := $(O_GENERATED)/nls
NLS_MO := $(addsuffix /LC_MESSAGES/leonos.mo,$(addprefix $(NLS_DIR)/,$(RELIEFOS_NLS_LANGS)))
NLS_MUSL_MO := $(addsuffix .UTF-8,$(addprefix $(O_GENERATED)/musl-locales/,$(RELIEFOS_NLS_LANGS)))
RELIEFOS_NLS_SOURCES := $(sort $(wildcard $(RELIEFOS_SRC)/userland/apps/*/*.c $(RELIEFOS_SRC)/userland/apps/*/*.cpp \
    $(RELIEFOS_SRC)/userland/apps/fltk_*.h \
    $(RELIEFOS_SRC)/userland/apps/*/*.h $(RELIEFOS_SRC)/userland/runtime/src/*.c $(RELIEFOS_SRC)/configs/nls/messages.c))
RELIEFOS_NLS_REL_SOURCES := $(patsubst $(RELIEFOS_SRC)/%,%,$(RELIEFOS_NLS_SOURCES))
RELIEFOS_SIG_nls := msgfmt=$(shell msgfmt --version 2>/dev/null | head -n1)|flags=-c --check --endianness=little|langs=$(RELIEFOS_NLS_LANGS)|sources=$(RELIEFOS_NLS_REL_SOURCES)
$(if $(RELIEFOS_PASSIVE),,$(eval $(call RELIEFOS_SIGNATURE_RULE,nls)))

define reliefos-nls-mo-rule
$(NLS_DIR)/$(1)/LC_MESSAGES/leonos.mo: $(RELIEFOS_NLS_PO)/$(1).po $(O_META)/nls.sig $(RELIEFOS_SRC)/mk/nls.mk
	$$(call RELIEFOS_LOG,MSGFMT,$$@)
	$$(Q)mkdir -p $$(@D)
	$$(Q)set -eu; trap 'rm -f $$@.tmp $$@.log' EXIT HUP INT TERM; \
	    if msgfmt -c --check --endianness=little -o $$@.tmp $$< >$$@.log 2>&1 && test ! -s $$@.log; then \
	        mv $$@.tmp $$@; \
	    else cat $$@.log >&2; exit 1; fi
endef
$(foreach lang,$(RELIEFOS_NLS_LANGS),$(eval $(call reliefos-nls-mo-rule,$(lang))))

.PHONY: nls nls-update
nls: $(NLS_MO)

define reliefos-musl-locale-rule
$(O_GENERATED)/musl-locales/$(1).UTF-8: $(RELIEFOS_SRC)/configs/locale/$(1).po $(RELIEFOS_SRC)/mk/nls.mk
	$$(call RELIEFOS_LOG,MSGFMT,$$@)
	$$(Q)mkdir -p $$(@D)
	$$(Q)msgfmt --check --endianness=little -o $$@ $$<
endef
$(foreach lang,$(RELIEFOS_NLS_LANGS),$(eval $(call reliefos-musl-locale-rule,$(lang))))

.PHONY: musl-locales
musl-locales: $(NLS_MUSL_MO)

# Explicit maintenance only: normal builds never rewrite the source corpus.
nls-update: $(RELIEFOS_NLS_EXTRACT)
	$(Q)mkdir -p $(NLS_DIR)
	$(Q)cd $(RELIEFOS_SRC) && $(abspath $(RELIEFOS_NLS_EXTRACT)) $(abspath $(NLS_DIR))/leonos.pot $(RELIEFOS_NLS_REL_SOURCES)
	$(Q)set -eu; cd $(RELIEFOS_SRC); for lang in $(RELIEFOS_NLS_LANGS); do \
	    $(abspath $(RELIEFOS_NLS_EXTRACT)) --merge $(RELIEFOS_NLS_PO)/$$lang.po $(abspath $(NLS_DIR))/$$lang.po $(RELIEFOS_NLS_REL_SOURCES); \
	    msgfmt -c --check --endianness=little -o /dev/null $(abspath $(NLS_DIR))/$$lang.po; \
	 done
	$(Q)cp $(NLS_DIR)/leonos.pot $(RELIEFOS_NLS_PO)/leonos.pot
	$(Q)set -eu; for lang in $(RELIEFOS_NLS_LANGS); do cp $(NLS_DIR)/$$lang.po $(RELIEFOS_NLS_PO)/$$lang.po; done
