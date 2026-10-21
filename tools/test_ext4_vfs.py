#!/usr/bin/env python3
import argparse
from pathlib import Path
import tempfile
from test_ext4_read import run

def main():
    p=argparse.ArgumentParser(); p.add_argument("--filesystem",choices=("ext2","ext4"),required=True)
    args=p.parse_args()
    with tempfile.TemporaryDirectory(prefix="reliefos-ext4-vfs-") as d:
        work=Path(d); binary=work/"test"; image=work/"image"; result=work/"result"
        run(["cc","-std=c11","-O1","-g","-fsanitize=address,undefined","-fno-sanitize-recover=all",
             "-ffunction-sections","-fdata-sections","-Wl,--gc-sections","-Ikernel/reliefnt/include",
             "-Iinclude","-Ikernel/reliefnt/include/uapi","-Ikernel/reliefnt/include",
             "tools/tests/ext4_vfs_test.c","kernel/reliefnt/fs/tmpfs/core.c",
             "kernel/reliefnt/lib/text_utf16.c","-o",str(binary)])
        features="none,filetype,large_file,dir_index"
        if args.filesystem=="ext4": features+=",extents,64bit,metadata_csum,extra_isize,flex_bg,has_journal"
        with image.open("wb") as f:f.truncate(64*1024*1024)
        run(["mke2fs","-q","-F","-t",args.filesystem,"-b","4096","-I","256","-O",features,str(image)])
        run([str(binary),str(image),str(result)])
        run(["e2fsck","-f","-n",str(result)])
if __name__=="__main__":main()
