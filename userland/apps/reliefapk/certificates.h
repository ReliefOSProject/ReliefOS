#ifndef RELIEFAPK_CERTIFICATES_H
#define RELIEFAPK_CERTIFICATES_H
#include "model.h"
namespace ra {
/* Targets are the signed CA sources and bundle packages. Real transactions require
 * root; previews can run as the logged-in user. */
int certificate_command(bool simulate);
/* Restore the fixed CA filename in an owned, private-writable directory.
 * Existing regular-file contents are preserved in a private unique backup. */
bool restore_ca_bundle_at(int directory_fd, const Text &bundle, Text &backup_name, Text &error);
/* Relay bounded APK output without repository URLs. A skipped transaction or
 * incomplete output is an error even when apk exits with status zero. */
bool relay_certificate_apk_output(int input_fd);
/* Missing/empty bundles can be recovered; symlinks, foreign ownership and
 * writable trust paths must be refused before any privileged network use. */
bool safe_ca_bundle_at(int directory_fd, unsigned long expected_uid, Text &error);
}
#endif
