#define N_(s) (s)

/* Strings supplied by shared code and dynamic application labels. */
static const char *const network_controller_indirect_messages[] = {
    N_("Authenticating"),
    N_("Export output"),
};

/* Static diagnostics cross an application boundary as untranslated data. */
static const char *const network_controller_diagnostic_messages[] = {
    N_("\nOperation canceled. Check service state before retrying.\n"),
    N_("\nOperation timed out. Check service state before retrying.\n"),
    N_("Cannot create command output pipe.\n"),
    N_("Cannot create network configuration directory"),
    N_("Cannot create network configuration temporary file"),
    N_("Cannot open network configuration directory"),
    N_("Cannot query network interfaces.\n"),
    N_("Cannot read network configuration"),
    N_("Cannot restore absent network profile"),
    N_("Cannot save network configuration"),
    N_("Cannot synchronize network configuration directory"),
    N_("Cannot synchronize restored network profile"),
    N_("Configuration changed outside this window. Reload it before saving."),
    N_("Configuration replaced but directory synchronization failed"),
    N_("DHCP renewal requires DHCP mode.\n"),
    N_("Invalid Ethernet interface"),
    N_("Network Controller diagnostics\n\nAdapters (live ioctl and counters):\n"),
    N_("Network configuration is locked or unsafe"),
    N_("Network configuration saved. Apply it to update the running network.\n"),
    N_("No previous network configuration backup is available."),
    N_("Not available.\n"),
    N_("Requested network information is absent or cannot be read.\n"),
    N_("The selected Ethernet interface is no longer present.\n"),
    N_("Unsafe configuration directory"),
    N_("Unsafe network configuration file"),
};
