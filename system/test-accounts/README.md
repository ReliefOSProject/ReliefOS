# Standalone Test Image Accounts

Only `image-vmdk` and the ordinary `image-iso` consume these fixtures, through
`tools/image_test_accounts.py`. They are not part of `system/rootfs`, shared
staging, the installer runtime or its installed-system payload.

- `test` / `test`: UID/GID 1000, supplementary `wheel` (GID 10) and `video`
  (GID 44) groups. `video` grants the test desktop DAC access to DRM devices;
  `wheel` alone controls the normal password-authenticated sudo policy.
- `root` / `root`: UID/GID 0.
- `nobody`: locked.

These are intentionally public test credentials. The yescrypt hashes have
independent random salts; the positive shadow change date avoids forcing a
password change at first login. sudo uses the normal password-authenticated
wheel policy and therefore asks for `test` when run by the test account.

The image-specific root gets an installed marker to require PAM login, and a
test-image marker for fakeroot to assign `/home/test` to 1000:1000. Rootfs account
files and root's home remain owned by root. The installer continues to provision
the passwords chosen by its user.
