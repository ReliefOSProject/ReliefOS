# SPDX-License-Identifier: Apache-2.0
"""Test-only oracle for bounded GRUB records and paired kernel boot slots.

These functions validate records and immutable payloads. Publication/fsync and
the firmware selector have separate integration tests; this codec alone makes
no claim about an interrupted update or a particular filesystem's durability.
"""
import hashlib
from pathlib import Path
import re

def contained(root, relative):
    """Keep the independent test oracle inside its disposable test tree."""
    if not re.fullmatch(r"[A-Za-z0-9_.+/-]+", relative) or relative.startswith("/"):
        raise ValueError("invalid test bundle path")
    parts = relative.split("/")
    if any(part in ("", ".", "..") for part in parts):
        raise ValueError("noncanonical test bundle path")
    path = Path(root)
    for part in parts:
        path /= part
        if path.is_symlink():
            raise ValueError("symlink test bundle member")
    if not path.is_file():
        raise ValueError("missing test bundle member")
    return path


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()

HEADER = b"# GRUB Environment Block\n"
SIZE = 1024
KEYS = ("generation", "release", "module_abi", "kernel_sha256", "initramfs_sha256")
MAX_GENERATION = 2147483647  # Also representable by 32-bit BIOS GRUB test.


def encode(fields):
    """Encode only canonical values; no descriptor contains executable syntax."""
    if set(fields) != set(KEYS) or type(fields["generation"]) is not int or not 0 < fields["generation"] <= MAX_GENERATION:
        raise ValueError("invalid kernel-slot fields or generation")
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+-]{0,30}", fields["release"]):
        raise ValueError("invalid kernel-slot release")
    for key in ("module_abi", "kernel_sha256", "initramfs_sha256"):
        if not re.fullmatch("[0-9a-f]{64}", fields[key]):
            raise ValueError("invalid kernel-slot digest")
    if not fields["release"].endswith(fields["module_abi"][:12]):
        raise ValueError("kernel-slot ABI does not match release")
    body = HEADER + "".join(f"reliefos_{key}={fields[key]}\n" for key in KEYS).encode("ascii")
    if len(body) >= SIZE:
        raise ValueError("kernel-slot descriptor is too large")
    return body + b"#" * (SIZE - len(body))


def decode(record):
    """Decode an exact-size environment block without evaluating its contents."""
    if len(record) != SIZE or not record.startswith(HEADER):
        raise ValueError("invalid kernel-slot environment")
    try:
        body, padding = record[len(HEADER):].split(b"#", 1)
        if padding != b"#" * len(padding):
            raise ValueError("noncanonical kernel-slot padding")
        lines = body.decode("ascii").splitlines()
        if len(lines) != len(KEYS):
            raise ValueError("incomplete kernel-slot descriptor")
        fields = {}
        for key, line in zip(KEYS, lines):
            prefix = "reliefos_" + key + "="
            if not line.startswith(prefix):
                raise ValueError("unexpected kernel-slot variable")
            fields[key] = line[len(prefix):]
        fields["generation"] = int(fields["generation"])
        if encode(fields) != record:
            raise ValueError("noncanonical kernel-slot descriptor")
        return fields
    except (UnicodeError, KeyError, TypeError) as error:
        raise ValueError("invalid kernel-slot bytes") from error


def checksum(slot, record):
    """Generate one fixed-size upstream sha256sum record for an exact slot."""
    if slot not in ("A", "B"):
        raise ValueError("invalid kernel-slot name")
    decode(record)
    return (hashlib.sha256(record).hexdigest() + f"  kernel-{slot}.env\n").encode("ascii")


def write_version(root, fields, kernel, archive):
    """Build a private test fixture independently of the native producer."""
    path = Path(root) / "reliefos/versions" / fields["release"] / fields["initramfs_sha256"]
    path.mkdir(parents=True)
    products = {"kernel.sys": kernel, "initramfs.cpio": archive, "boot.env": encode({**fields, "generation": 1})}
    for name, data in products.items():
        (path / name).write_bytes(data)
    (path / "SHA256SUMS").write_text("".join(hashlib.sha256(data).hexdigest() + "  " + name + "\n"
                                            for name, data in products.items()))
    return path


def select(root):
    """Choose the newest complete slot, ignoring incomplete or corrupt slots."""
    root = Path(root)
    valid = []
    for slot in ("A", "B"):
        try:
            path = f"reliefos/state/kernel-{slot}"
            record = contained(root, path + ".env").read_bytes()
            if contained(root, path + ".sha256").read_bytes() != checksum(slot, record):
                continue
            fields = decode(record)
            prefix = "reliefos/versions/" + fields["release"] + "/" + fields["initramfs_sha256"] + "/"
            if any(digest(contained(root, prefix + name)) != fields[key] for name, key in
                   (("kernel.sys", "kernel_sha256"), ("initramfs.cpio", "initramfs_sha256"))):
                continue
            template = contained(root, prefix + "boot.env").read_bytes()
            if decode(template) != {**fields, "generation": 1}:
                continue
            expected_sums = fields["kernel_sha256"] + "  kernel.sys\n" + fields["initramfs_sha256"] + \
                "  initramfs.cpio\n" + hashlib.sha256(template).hexdigest() + "  boot.env\n"
            if contained(root, prefix + "SHA256SUMS").read_bytes() != expected_sums.encode():
                continue
            valid.append({**fields, "slot": slot})
        except (OSError, ValueError, KeyError, TypeError):
            continue
    if not valid:
        raise ValueError("no complete kernel boot slot")
    if len(valid) == 2 and valid[0]["generation"] == valid[1]["generation"] and \
       {k: valid[0][k] for k in KEYS} != {k: valid[1][k] for k in KEYS}:
        raise ValueError("ambiguous kernel-slot generation")
    return max(valid, key=lambda fields: fields["generation"])
