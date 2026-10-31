#!/usr/bin/env python3
"""ReliefOS syscall wrapper and SDK sound-header export contracts."""
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
def test_native_syscall_wrapper():
    source = (ROOT / "userland/runtime/src/syscall.S").read_text()
    assert "syscall" in source and "int $0x80" not in source

def test_audio_export_closure():
    needed = {"include/uapi/sound/asound.h", "include/uapi/sound/tlv.h",
              "include/uapi/linux/soundcard.h", "include/uapi/linux/patchkey.h",
              "include/uapi/linux/time.h", "include/uapi/linux/types.h",
              "include/uapi/linux/ioctl.h"}
    exported = {line.strip() for line in (ROOT / "configs/header-export.list").read_text().splitlines()
                if line.strip() and not line.startswith("#")}
    assert needed <= exported, f"SDK export missing sound closure {needed - exported}"

if __name__ == "__main__":
    for test in (test_native_syscall_wrapper, test_audio_export_closure):
        test()
        print(f"PASS {test.__name__}")
