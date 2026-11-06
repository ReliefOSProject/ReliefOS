"""Public test credentials for standalone ISO/VMDK images, never installer seeds."""
import os
from pathlib import Path
import shutil

SEEDS = Path(__file__).resolve().parents[1] / "system/test-accounts"
MARKER = "etc/reliefos/test-image"
PROFILE = "leonos-standalone-test-v1\n"


def seed_test_accounts(root: Path) -> None:
    for name in ("passwd", "shadow", "group", "gshadow"):
        destination = root / "etc" / name
        if destination.is_symlink():
            raise ValueError(f"test account file is a symlink: {destination}")
        shutil.copyfile(SEEDS / name, destination)
        destination.chmod(0o600 if name in ("shadow", "gshadow") else 0o644)
    policy = root / "etc/sudoers"
    if policy.is_symlink() or "%wheel ALL=(ALL:ALL) ALL" not in policy.read_text().splitlines():
        raise ValueError("standalone test images require the password-authenticated wheel policy")
    policy.chmod(0o440)
    for name in ("root", "home/test"):
        home = root / name
        if home.is_symlink():
            raise ValueError(f"test home is a symlink: {home}")
        home.mkdir(parents=True, exist_ok=True)
        shutil.copytree(root / "etc/skel", home, symlinks=True, dirs_exist_ok=True)
        home.chmod(0o700)
    # The existing session launcher requires this marker to enforce PAM login.
    settings = root / "etc/reliefos"
    for name, text in (("test-image", PROFILE), ("installed", "test-image=1\n")):
        path = settings / name
        if path.is_symlink():
            raise ValueError(f"test image marker is a symlink: {path}")
        path.write_text(text, encoding="ascii")
        path.chmod(0o644)


def apply_test_home_ownership(root: Path) -> None:
    """Run inside fakeroot after the normal root:root ownership pass."""
    marker = root / MARKER
    if not marker.exists():
        return
    if marker.is_symlink() or marker.read_text() != PROFILE:
        raise ValueError("invalid standalone test account profile")
    if not os.environ.get("FAKEROOTKEY"):
        raise RuntimeError("test home ownership requires fakeroot")
    home = root / "home/test"
    if home.is_symlink() or not home.is_dir():
        raise ValueError("invalid standalone test home")
    os.chown(home, 1000, 1000, follow_symlinks=False)
    for directory, dirs, files in os.walk(home, followlinks=False):
        for name in dirs + files:
            os.chown(Path(directory) / name, 1000, 1000, follow_symlinks=False)
