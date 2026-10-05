# Test targets.
#
# The core harness is POSIX shell and C: the acceptance rule for this migration
# may not depend on the interpreter it is auditing (plan section 2).

RELIEFOS_TEST_TMP := $(if $(TMPDIR),$(TMPDIR),/tmp)/reliefos-tests-$(shell id -u)

# C unit tests for the shared host primitives and the JSON reader.
RELIEFOS_HOST_TEST_BINS := $(O_HOST)/tests/test_common $(O_HOST)/tests/test_json
RELIEFOS_HOST_TEST_SANITISED := $(O_HOST)/tests-sanitised/test_common \
	$(O_HOST)/tests-sanitised/test_json
RELIEFOS_HOST_TEST_BINS += $(O_HOST)/tests/test_locale_conf
RELIEFOS_HOST_TEST_SANITISED += $(O_HOST)/tests-sanitised/test_locale_conf

# Shell contract tests. test-bootstrap.sh is the public entry-point surface.
# test-brand-identity.sh is the ReliefOS/ReliefNT rename gate (plan task 1) and
# is picked up by this wildcard on purpose: every test-build run audits names.
RELIEFOS_BUILD_TESTS := $(sort $(wildcard $(RELIEFOS_SRC)/tests/build/test-*.sh))
# Suites that are too long to run on every change but must not be forgotten:
# repeated -j1/-j8 comparisons and, later, guest boots. Plan section 13 forbids
# presenting an unrun long test as a skipped pass, so they are a separate goal.
RELIEFOS_LONG_TESTS := $(sort $(wildcard $(RELIEFOS_SRC)/tests/long/test-*.sh))

# The plain and sanitised suites are separate goals so `test-tools` keeps a
# single recipe: the plan requires ASan/UBSan at the tool boundary, not merely a
# clean compile.
.PHONY: reliefos-test-tools-plain reliefos-test-tools-sanitised \
	leonos-test-tools-plain leonos-test-tools-sanitised

test-tools: reliefos-test-tools-plain reliefos-test-tools-sanitised

reliefos-test-tools-plain: $(RELIEFOS_HOST_TEST_BINS)
	@set -eu; for test_binary in $(RELIEFOS_HOST_TEST_BINS); do \
	    $(call RELIEFOS_LOG_SHELL,RUN,$$test_binary); \
	    $$test_binary; \
	done

reliefos-test-tools-sanitised: $(RELIEFOS_HOST_TEST_SANITISED)
	@set -eu; for test_binary in $(RELIEFOS_HOST_TEST_SANITISED); do \
	    $(call RELIEFOS_LOG_SHELL,RUN,$$test_binary); \
	    ASAN_OPTIONS=detect_leaks=1 \
	    UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 $$test_binary; \
	done

# Contract tests get the built tool and the lock file by name so they never
# reach for a stale copy left in the output tree.
#
# A suite can report ok lines while still exiting 0 on a failure it counted
# itself, and it can exit non-zero after printing nothing. Both are checked: the
# status *and* any `FAIL - ` line in the captured report.
#
# $(call RELIEFOS_RUN_CONTRACT_TESTS,tests...)
define RELIEFOS_RUN_CONTRACT_TESTS
	set -eu; for contract_test in $(1); do \
	    $(call RELIEFOS_LOG_SHELL,RUN,$$contract_test); \
	    report=$$(mktemp); \
	    if RELIEFOS_DEPS='$(RELIEFOS_DEPS_TOOL)' \
	       LEONOS_DEPS='$(RELIEFOS_DEPS_TOOL)' \
	       RELIEFOS_LOCK='$(RELIEFOS_SRC)/configs/dependencies.lock.json' \
	       LEONOS_LOCK='$(RELIEFOS_SRC)/configs/dependencies.lock.json' \
	       RELIEFOS_EMIT='$(RELIEFOS_EMIT)' \
	       LEONOS_EMIT='$(RELIEFOS_EMIT)' \
	       sh $$contract_test >$$report 2>&1; then status=0; else status=1; fi; \
	    cat $$report; \
	    if grep -q 'FAIL - ' $$report; then status=1; fi; \
	    rm -f $$report; \
	    if [ $$status -ne 0 ]; then \
	        printf 'not ok - %s reported a failure\n' $$contract_test; exit 1; \
	    fi; \
	done
endef

# Keep the former private goal names available to scripts during the rename.
leonos-test-tools-plain: reliefos-test-tools-plain
leonos-test-tools-sanitised: reliefos-test-tools-sanitised

test-build: test-tools $(RELIEFOS_HOST_TOOLS)
	@$(call RELIEFOS_RUN_CONTRACT_TESTS,$(RELIEFOS_BUILD_TESTS))

test-long: $(RELIEFOS_HOST_TOOLS)
	@$(call RELIEFOS_RUN_CONTRACT_TESTS,$(RELIEFOS_LONG_TESTS))

# Audio userland is an explicit host gate because it needs the staged ALSA and
# Nuked OPL3 products.  Keep it separate from the generic shell suite so a
# missing audio staging tree is reported as a real prerequisite failure rather
# than silently skipped.
.PHONY: test-audio
test-audio:
	@python3 $(RELIEFOS_SRC)/tools/test_audio_userland.py controls settings doom-output doom-mixer doom-music

test: test-tools test-build

$(O_HOST)/obj/tests/host/%.c.o: $(RELIEFOS_SRC)/tests/host/%.c $(O_META)/host-cc.sig
	$(Q)mkdir -p $(dir $@)
	$(call RELIEFOS_LOG,HOSTCC,$<)
	$(Q)$(HOSTCC) $(RELIEFOS_STRICT_WARNINGS) -I$(RELIEFOS_SRC)/tests/host \
	    $(RELIEFOS_HOST_INCLUDES) $(HOST_CFLAGS) -MMD -MF $@.d -c $< -o $@

$(O_HOST)/obj/tests-sanitised/host/%.c.o: $(RELIEFOS_SRC)/tests/host/%.c $(O_META)/host-cc-sanitised.sig
	$(Q)mkdir -p $(dir $@)
	$(call RELIEFOS_LOG,HOSTCC,$<)
	$(Q)$(HOSTCC) $(RELIEFOS_STRICT_WARNINGS) $(RELIEFOS_SANITISE) \
	    -I$(RELIEFOS_SRC)/tests/host $(RELIEFOS_HOST_INCLUDES) -g -O1 \
	    -MMD -MF $@.d -c $< -o $@

$(O_HOST)/obj/tests-sanitised/tools/host/%.c.o: $(RELIEFOS_SRC)/tools/host/%.c \
	$(O_META)/host-cc-sanitised.sig
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOSTCC) $(RELIEFOS_STRICT_WARNINGS) $(RELIEFOS_SANITISE) -g -O1 \
	    $(RELIEFOS_HOST_INCLUDES) -MMD -MF $@.d -c $< -o $@

$(O_HOST)/tests/test_common: $(O_HOST)/obj/tests/host/test_common.c.o $(RELIEFOS_HOST_COMMON_OBJS)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOSTCC) $(HOST_CFLAGS) $(HOST_LDFLAGS) $^ -o $@

$(O_HOST)/tests/test_json: $(O_HOST)/obj/tests/host/test_json.c.o $(RELIEFOS_JSON_OBJ) \
	$(RELIEFOS_HOST_COMMON_OBJS)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOSTCC) $(HOST_CFLAGS) $(HOST_LDFLAGS) $^ -o $@

RELIEFOS_HOST_SANITISED_OBJS := $(patsubst $(O_HOST)/obj/tools/host/%.c.o, \
	$(O_HOST)/obj/tests-sanitised/tools/host/%.c.o,$(RELIEFOS_HOST_COMMON_OBJS))

$(O_HOST)/tests-sanitised/test_common: $(O_HOST)/obj/tests-sanitised/host/test_common.c.o \
	$(RELIEFOS_HOST_SANITISED_OBJS)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOSTCC) $(RELIEFOS_SANITISE) -g -O1 $^ -o $@

$(O_HOST)/tests-sanitised/test_json: $(O_HOST)/obj/tests-sanitised/host/test_json.c.o \
	$(O_HOST)/obj/tests-sanitised/tools/host/manifest/json.c.o $(RELIEFOS_HOST_SANITISED_OBJS)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOSTCC) $(RELIEFOS_SANITISE) -g -O1 $^ -o $@

RELIEFOS_SIG_host-cc-sanitised := argv=$(HOSTCC) $(RELIEFOS_STRICT_WARNINGS) $(RELIEFOS_SANITISE) -g -O1|path=$(reliefos_host_tool_path)|identity=$(reliefos_host_tool_identity)
$(if $(RELIEFOS_PASSIVE),,$(eval $(call RELIEFOS_SIGNATURE_RULE,host-cc-sanitised)))

-include $(shell find $(O_HOST)/obj/tests $(O_HOST)/obj/tests-sanitised -name '*.o.d' 2>/dev/null)
.PHONY: test test-tools test-build test-long

$(O_HOST)/obj/tests/host/test_locale_conf.c.o: $(RELIEFOS_SRC)/tests/host/test_locale_conf.c $(RELIEFOS_SRC)/userland/runtime/src/locale_conf.h $(O_META)/host-cc.sig
	$(Q)mkdir -p $(@D)
	$(call RELIEFOS_LOG,HOSTCC,$<)
	$(Q)$(HOSTCC) $(RELIEFOS_STRICT_WARNINGS) $(HOST_CFLAGS) -I$(RELIEFOS_SRC)/userland/runtime/src $(RELIEFOS_HOST_INCLUDES) -MMD -MF $@.d -c $< -o $@

$(O_HOST)/obj/tests/host/userland/locale_conf.c.o: $(RELIEFOS_SRC)/userland/runtime/src/locale_conf.c $(RELIEFOS_SRC)/userland/runtime/src/locale_conf.h $(O_META)/host-cc.sig
	$(Q)mkdir -p $(@D)
	$(call RELIEFOS_LOG,HOSTCC,$<)
	$(Q)$(HOSTCC) $(RELIEFOS_STRICT_WARNINGS) $(HOST_CFLAGS) -I$(RELIEFOS_SRC)/userland/runtime/src $(RELIEFOS_HOST_INCLUDES) -MMD -MF $@.d -c $< -o $@

$(O_HOST)/tests/test_locale_conf: $(O_HOST)/obj/tests/host/test_locale_conf.c.o $(O_HOST)/obj/tests/host/userland/locale_conf.c.o
	$(Q)mkdir -p $(@D)
	$(Q)$(HOSTCC) $(HOST_CFLAGS) $(HOST_LDFLAGS) $^ -o $@

$(O_HOST)/obj/tests-sanitised/host/test_locale_conf.c.o: $(RELIEFOS_SRC)/tests/host/test_locale_conf.c $(RELIEFOS_SRC)/userland/runtime/src/locale_conf.h $(O_META)/host-cc-sanitised.sig
	$(Q)mkdir -p $(@D)
	$(call RELIEFOS_LOG,HOSTCC,$<)
	$(Q)$(HOSTCC) $(RELIEFOS_STRICT_WARNINGS) $(RELIEFOS_SANITISE) -g -O1 -I$(RELIEFOS_SRC)/userland/runtime/src $(RELIEFOS_HOST_INCLUDES) -MMD -MF $@.d -c $< -o $@

$(O_HOST)/obj/tests-sanitised/host/userland/locale_conf.c.o: $(RELIEFOS_SRC)/userland/runtime/src/locale_conf.c $(RELIEFOS_SRC)/userland/runtime/src/locale_conf.h $(O_META)/host-cc-sanitised.sig
	$(Q)mkdir -p $(@D)
	$(call RELIEFOS_LOG,HOSTCC,$<)
	$(Q)$(HOSTCC) $(RELIEFOS_STRICT_WARNINGS) $(RELIEFOS_SANITISE) -g -O1 -I$(RELIEFOS_SRC)/userland/runtime/src $(RELIEFOS_HOST_INCLUDES) -MMD -MF $@.d -c $< -o $@

$(O_HOST)/tests-sanitised/test_locale_conf: $(O_HOST)/obj/tests-sanitised/host/test_locale_conf.c.o $(O_HOST)/obj/tests-sanitised/host/userland/locale_conf.c.o
	$(Q)mkdir -p $(@D)
	$(Q)$(HOSTCC) $(RELIEFOS_SANITISE) -g -O1 $^ -o $@
