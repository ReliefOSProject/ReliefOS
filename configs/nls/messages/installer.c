#define N_(s) (s)

/* Progress and failure strings passed to translated installer callbacks. */
static const char *const installer_progress_messages[] = {
    N_("Mounting target filesystems"),
    N_("Mount failed"),
};

/* Strings supplied by shared code and dynamic application labels. */
static const char *const installer_indirect_messages[] = {
    N_("Language"),
    N_("License"),
    N_("Welcome"),
    N_("Installation mode"),
    N_("Target disk"),
    N_("Bootloaders"),
    N_("Configure partitions"),
    N_("Account and system options"),
    N_("Review installation"),
    N_("Installation progress"),
    N_("Installation finished"),
    N_("Confirm password"),
    N_("Delete"),
    N_("Format"),
    N_("Password:"),
    N_("Ready"),
    N_("Rename"),
    N_("none"),
    N_("Use as ESP"),
    N_("Use as root"),
    N_("New GPT"),
    N_("Standard user name"),
    N_("Standard user password"),
};
