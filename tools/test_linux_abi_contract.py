#!/usr/bin/env python3
"""Host-side contract checks for the native x86-64 Linux syscall ABI."""

from pathlib import Path
import json
import re


ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def macro(text: str, name: str) -> int:
    match = re.search(rf"^#define {re.escape(name)}\s+(0x[0-9a-fA-F]+|\d+)", text, re.M)
    assert match, f"missing {name}"
    return int(match.group(1), 0)


def test_linux_numbers_and_flags() -> None:
    syscall = read("kernel/reliefnt/include/uapi/linux/syscall.h")
    fcntl = read("kernel/reliefnt/include/uapi/linux/fcntl.h")
    assert macro(syscall, "__NR_pause") == 34
    assert macro(syscall, "__NR_arch_prctl") == 158
    assert macro(syscall, "__NR_set_tid_address") == 218
    assert macro(syscall, "__NR_exit_group") == 231
    assert macro(syscall, "__NR_gettid") == 186
    assert macro(fcntl, "LINUX_O_NONBLOCK") == 0x800
    assert macro(fcntl, "LINUX_F_DUPFD_CLOEXEC") == 1030


def test_native_syscall_entry_and_stack_protocol() -> None:
    syscall_asm = read("userland/runtime/src/syscall.S")
    boot_asm = read("kernel/reliefnt/arch/x86_64/boot.S")
    gdt = read("kernel/reliefnt/arch/x86_64/gdt.c")
    userland = read("kernel/reliefnt/kernel/exec/userland.c")
    assert "syscall" in syscall_asm and "int $0x80" not in syscall_asm
    assert "x86_64_syscall_entry" in boot_asm
    assert "X86_IA32_EFER" in gdt and "read_msr(X86_IA32_EFER) | 1ULL" in gdt
    assert "AT_PHDR" in userland and "AT_RANDOM" in userland


def test_contract_fixes_are_present() -> None:
    syscall_h = read("kernel/reliefnt/kernel/reliefnt/include/reliefnt/syscall.h")
    process = read("kernel/reliefnt/kernel/reliefnt/syscall_process.c")
    syscall = read("kernel/reliefnt/kernel/reliefnt/syscall.c")
    ipc = read("kernel/reliefnt/kernel/reliefnt/syscall_ipc.c")
    assert "LINUX_SYS_PAUSE" in syscall_h
    assert "LINUX_SYS_NICE __NR_nice" not in syscall_h
    assert "SIG_BLOCK=0" in process or "SIG_BLOCK" in process
    assert "case LINUX_SYS_PAUSE" in syscall
    assert "flags & ~(uint32_t)(RELIEFOS_O_NONBLOCK | RELIEFOS_O_CLOEXEC)" in ipc
    assert "return -RELIEFOS_EBADF" in syscall


def test_audio_export_closure() -> None:
    # Host /usr/include can conceal a missing dependency. Both root and kernel
    # export lists must publish the complete public sound include closure.
    needed = {"include/uapi/sound/asound.h", "include/uapi/sound/tlv.h",
              "include/uapi/linux/soundcard.h", "include/uapi/linux/patchkey.h",
              "include/uapi/linux/time.h", "include/uapi/linux/types.h",
              "include/uapi/linux/ioctl.h"}
    for path in ("configs/header-export.list", "kernel/reliefnt/configs/header-export.list"):
        exported = {line.strip() for line in read(path).splitlines() if not line.startswith("#")}
        assert needed <= exported, f"{path}: missing sound closure {needed - exported}"


def test_audio_support_manifest() -> None:
    manifest = json.loads(read("docs/audio-abi-support.json"))
    assert manifest["uapi"] == "Linux v6.14 native x86-64"
    expected = set(re.findall(r"^#define\s+(SNDRV_(?:PCM|CTL)_IOCTL_\w+)",
                              read("kernel/reliefnt/include/uapi/sound/asound.h"), re.M))
    expected |= set(re.findall(r"^#define\s+((?:SNDCTL_DSP|SOUND_PCM_(?:READ|WRITE))_\w+)",
                              read("kernel/reliefnt/include/uapi/linux/soundcard.h"), re.M))
    assert expected == set(manifest["ioctls"]), f"manifest ioctl set differs: {expected ^ set(manifest['ioctls'])}"
    for name, entry in manifest["ioctls"].items():
        assert isinstance(entry["supported"], bool), name
        for field in ("access", "state", "errno", "mmap", "evidence"):
            assert entry[field], (name, field)
    for path in re.findall(r"tools/(?:test_\w+\.py|tests/\w+\.c)", json.dumps(manifest)):
        assert (ROOT / path).is_file(), f"manifest evidence file missing: {path}"
    for category in ("guest_runtime", "upstream_alsa", "physical_hardware"):
        assert manifest["validation"][category] is False, category
    assert manifest["ioctls"]["SNDRV_PCM_IOCTL_WRITEN_FRAMES"]["supported"] is False
    assert manifest["ioctls"]["SNDRV_CTL_IOCTL_RAWMIDI_INFO"]["supported"] is False


if __name__ == "__main__":
    tests = [test_linux_numbers_and_flags,
             test_native_syscall_entry_and_stack_protocol,
             test_contract_fixes_are_present, test_audio_export_closure,
             test_audio_support_manifest]
    for test in tests:
        test()
        print(f"PASS {test.__name__}")
