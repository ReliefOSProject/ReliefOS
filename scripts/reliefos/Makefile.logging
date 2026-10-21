# Central build-log formatting for GNU Make recipes.
#
# RELIEFOS_LOG is for a standalone recipe line and follows V=1 through Q.
# RELIEFOS_LOG_SHELL is for a command embedded in a shell loop or compound
# recipe, where Make's leading @ must not become part of the shell command.

RELIEFOS_LOG_FORMAT := '  %-8s %s\n'
RELIEFOS_LOG_COMMAND = printf $(RELIEFOS_LOG_FORMAT)
RELIEFOS_SHELL_LOG := $(RELIEFOS_SRC)/scripts/logging.sh

# $(call RELIEFOS_LOG,label,message)
define RELIEFOS_LOG
$(Q)$(RELIEFOS_LOG_COMMAND) "$(1)" "$(2)"
endef

# $(call RELIEFOS_LOG_SHELL,label,message)
define RELIEFOS_LOG_SHELL
$(RELIEFOS_LOG_COMMAND) "$(1)" "$(2)"
endef
