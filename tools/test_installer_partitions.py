"""Read real installer GPT output with an independent standard tool."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import tempfile
import uuid

ROOT = Path(__file__).resolve().parents[1]


def main():
    """Compile the real planner/writer, preserving all negative-case image bytes."""
    with tempfile.TemporaryDirectory(prefix="installer-gpt-") as directory:
        work = Path(directory)
        executable = work / "partitions"
        subprocess.run([
            "cc", "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", "-Wl,--wrap=open,--wrap=pwrite,--wrap=fsync",
            "-idirafter", "userland/runtime/include", "-idirafter", "include",
            "-Ikernel/reliefnt/include", "-Ikernel/reliefnt/include/uapi",
            "tools/tests/installer_partitions_test.c",
            "userland/apps/installer/installer_partitions.c", "-o", executable,
        ], cwd=ROOT, check=True)
        for case in ("small", "readonly", "mounted", "4kn", "badplan", "no-entropy", "normal", "manual", "old-fat", "clear-write-fail", "clear-sync-fail"):
            image = work / f"{case}.raw"
            with image.open("wb") as stream:
                stream.truncate((192 if case == "small" else 384) * 1024**2)
            if case == "old-fat":
                old = work / "old-esp.fat"
                with old.open("wb") as stream:
                    stream.truncate(128 * 1024**2)
                subprocess.run(["mkfs.fat", "-F", "32", old], check=True, stdout=subprocess.DEVNULL)
                with old.open("rb") as source, image.open("r+b") as target:
                    target.seek(1024**2)
                    target.write(source.read(1024**2))
                    target.seek(2 * 1024**2)
                    target.write(b"KEEP-BEYOND-BIOS-EXTENT")
            before = hashlib.file_digest(image.open("rb"), "sha256").hexdigest()
            subprocess.run([executable, image, case], check=True,
                           env=dict(os.environ, UBSAN_OPTIONS="halt_on_error=1"))
            if case in ("clear-write-fail", "clear-sync-fail"):
                # GPT erasure was confirmed; a physical partial write is not
                # rolled back, but no partitions/filesystems may be published.
                table = json.loads(subprocess.check_output(["sfdisk", "--json", image]))["partitiontable"]
                assert table.get("partitions", []) == []
                continue
            if case not in ("normal", "manual", "old-fat"):
                assert before == hashlib.file_digest(image.open("rb"), "sha256").hexdigest(), case
                continue
            if case == "old-fat":
                with image.open("rb") as stream:
                    stream.seek(1024**2)
                    assert stream.read(1024**2) == bytes(1024**2), "Old FAT signature/data survived inside the new BIOS Boot extent"
                    assert stream.read(len(b"KEEP-BEYOND-BIOS-EXTENT")) == b"KEEP-BEYOND-BIOS-EXTENT"
            table = json.loads(subprocess.check_output(["sfdisk", "--json", image]))["partitiontable"]
            parts = table["partitions"]
            if case == "manual":
                assert len(parts) == 4
                assert [p["start"] for p in parts] == [2048, 10240, 12288, 274432]
                assert parts[1]["type"].upper() == "21686148-6449-6E6F-744E-656564454649"
                assert parts[2]["type"].upper() == "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
                assert parts[3]["type"].upper() == "0FC63DAF-8483-4772-8E79-3D69D8477DE4"
                subprocess.run(["sfdisk", "--verify", image], check=True)
                continue
            assert table["label"] == "gpt" and len(parts) == 3
            assert [(p["start"], p["size"]) for p in parts[:2]] == [(2048, 2048), (4096, 262144)]
            assert parts[2]["start"] == 266240
            assert [p["type"].upper() for p in parts] == [
                "21686148-6449-6E6F-744E-656564454649",
                "C12A7328-F81F-11D2-BA4B-00A0C93EC93B",
                "0FC63DAF-8483-4772-8E79-3D69D8477DE4"]
            assert len({p["uuid"] for p in parts}) == 3
            assert uuid.UUID(table["id"]).version == 4
            assert all(uuid.UUID(p["uuid"]).version == 4 for p in parts)
            subprocess.run(["sfdisk", "--verify", image], check=True)
            second = work / "same-capacity.raw"
            with second.open("wb") as stream:
                stream.truncate(image.stat().st_size)
            subprocess.run([executable, second, "normal"], check=True)
            other = json.loads(subprocess.check_output(["sfdisk", "--json", second]))["partitiontable"]
            assert table["id"] != other["id"], "same-size installations reused a disk GUID"
            assert not ({p["uuid"] for p in parts} & {p["uuid"] for p in other["partitions"]}), "partition GUIDs reused across disks"
        print("PASS installer GPT: real primary/backup tables, BIOS/ESP/root geometry, UUIDs, NVMe paths and unchanged refusal cases")


if __name__ == "__main__":
    main()
