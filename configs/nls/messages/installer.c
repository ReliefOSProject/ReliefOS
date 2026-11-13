#define N_(s) (s)

/* Progress and failure strings passed to translated installer callbacks. */
static const char *const installer_progress_messages[] = {
    N_("Mounting target filesystems"),
    N_("Mount failed"),
};

/* Strings supplied by shared code and dynamic application labels. */
static const char *const installer_indirect_messages[] = {
    N_("Confirm password"),
    N_("Delete"),
    N_("Format"),
    N_("Password:"),
    N_("Ready"),
    N_("Rename"),
    N_("none"),
};
