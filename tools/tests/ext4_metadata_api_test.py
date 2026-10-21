#!/usr/bin/env python3
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from test_ext4_read import ROOT, STORAGE, FS, run

def main():
    with tempfile.TemporaryDirectory(prefix="reliefos-ext4-xattr-") as d:
        w=Path(d); binary=w/"test"; stage=w/"stage"; stage.mkdir(); (stage/"payload").touch()
        run(["cc","-std=c11","-O1","-g","-D_GNU_SOURCE","-fsanitize=address,undefined",
             "-fno-sanitize-recover=all","-ffunction-sections","-fdata-sections","-Wl,--gc-sections",
             "-DRELIEFOS_STORAGE_STANDALONE_TU","-Ikernel/reliefnt/include","-Iinclude",
             "-Ikernel/reliefnt/include/uapi","-Ikernel/reliefnt/include",
             "-include",str(STORAGE/"storage_internal.h"),"tools/tests/ext4_xattr_test.c",
             *[str(FS/f"storage_ext4_{n}.c") for n in
               ("format","checksum","cache","alloc","extent","ops","journal","dir","xattr")],"-o",str(binary)])
        for bs in (1024,4096):
            for kind,features in (("ext2","none,filetype,ext_attr"),
                ("ext4","none,filetype,extents,64bit,metadata_csum,extra_isize,ext_attr,has_journal")):
                image=w/"base"; result=w/"result"
                with image.open("wb") as f:f.truncate(32*1024*1024)
                run(["mke2fs","-q","-F","-t",kind,"-b",str(bs),"-I","256","-O",features,"-d",str(stage),str(image)])
                run(["debugfs","-w","-R","ea_set /payload user.linux original",str(image)],capture_output=True)
                stat=run(["debugfs","-R","stat /payload",str(image)],capture_output=True)
                ino=re.search(r"Inode:\s+(\d+)",stat.stdout)[1]
                run([str(binary),str(image),ino,str(result)])
                run(["e2fsck","-f","-n",str(result)])
                value=w/"value"
                run(["debugfs","-R",f"ea_get -f {value} /payload user.big",str(result)],capture_output=True)
                assert value.read_bytes()==bytes((i*19+23)%251 for i in range(bs-128))
                value.unlink()
                run(["debugfs","-R",f"ea_get -f {value} /payload user.linux",str(result)],capture_output=True)
                assert value.read_bytes()==b"replaced"
                print(f"PASS independent Linux xattr oracle {kind} bs={bs}",flush=True)
    print("PASS ext4 metadata API suite")
if __name__=="__main__":main()
