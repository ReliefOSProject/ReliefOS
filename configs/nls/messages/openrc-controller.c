#define N_(s) (s)

/* Strings supplied by shared code and dynamic application labels. */
static const char *const openrc_controller_indirect_messages[] = {
    N_("Advanced"),
    N_("Authenticating"),
    N_("Disable"),
    N_("Enable"),
    N_("failed"),
};

/* Static diagnostics cross an application boundary as untranslated data. */
static const char *const openrc_controller_diagnostic_messages[] = {
    N_("\nOpenRC status:\n"),
    N_("\nOperation canceled. Check service state before retrying.\n"),
    N_("\nOperation timed out after 180 seconds. Check service state before retrying.\n"),
    N_("\nUnassigned services:\n"),
    N_("Backup path is unsafe."),
    N_("Cannot change runlevel: it may exist, be active, referenced, nonempty, or unsafe."),
    N_("Cannot create command output pipe.\n"),
    N_("Cannot create temporary configuration file."),
    N_("Cannot open configuration (symbolic links are not allowed)."),
    N_("Cannot read OpenRC services and runlevels.\n"),
    N_("Cannot read service script.\n"),
    N_("Configuration changed externally. Reload before saving."),
    N_("Configuration changed while saving. Reload before saving."),
    N_("Configuration directory is missing or unsafe."),
    N_("Configuration is too large or contains NUL bytes."),
    N_("Configuration lock is unsafe or another editor is saving. Retry after it finishes."),
    N_("Configuration must be a safe, readable text file up to 256 KiB."),
    N_("Configuration replaced, but directory synchronization failed. Check disk status."),
    N_("Configuration saved. Previous contents: .openrc-controller.bak\n"),
    N_("Configuration write or atomic replacement failed."),
    N_("Invalid configuration path."),
    N_("Invalid or unsupported OpenRC request."),
    N_("Log is absent or cannot be read. Enable OpenRC rc_logger or the system logger if needed.\n"),
    N_("NUL bytes are not allowed in a request."),
    N_("No configuration backup is available."),
    N_("OpenRC Controller diagnostics\n\nService membership:\n"),
    N_("Runlevel updated.\n"),
    N_("This request does not execute a command."),
};
