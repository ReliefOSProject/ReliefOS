"""Host-test linkage to matching host-native libraries (the target uses signed APKs)."""
import os
from pathlib import Path

# Use libc's sys/mount.h declarations without the conflicting guest MNT_* macros.
# Keep ReliefNT's UAPI ahead of system headers for its prefixed fcntl ABI.
INSTALLER_HOST_INCLUDES = [
    "-DRELIEFOS_UAPI_LINUX_MOUNT_H", "-Ikernel/reliefnt/include",
    "-Ikernel/reliefnt/include/uapi", "-idirafter", "include",
    "-idirafter", "userland/runtime/include",
]


def dependency_root():
    root = Path(os.environ.get("RELIEFOS_TEST_DEP_ROOT", Path(__file__).resolve().parents[1] / "out/x86_64/release/host/runtime-development"))
    if not (root / "usr/include/mbedtls/ssl.h").is_file() or not (root / "usr/lib/libmbedcrypto.a").is_file():
        raise RuntimeError("Run make host-runtime-development first, or set RELIEFOS_TEST_DEP_ROOT to its host-native test stage")
    return root


def crypto_flags(*libraries):
    root = dependency_root()
    return ["-I" + str(root / "usr/include"), *[str(root / "usr/lib" / ("lib" + name + ".a")) for name in (libraries or ("mbedcrypto",))], "-pthread"]
