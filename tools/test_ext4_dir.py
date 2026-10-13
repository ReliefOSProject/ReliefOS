#!/usr/bin/env python3
"""Native HTREE and namespace operations with Linux e2fsck as oracle."""
import argparse
from pathlib import Path
import tempfile
import subprocess
from test_ext4_read import ROOT, STORAGE, FS, run

def main():
    p=argparse.ArgumentParser(description=__doc__); p.add_argument("--count",type=int,default=10000)
    args=p.parse_args()
    with tempfile.TemporaryDirectory(prefix="reliefos-ext4-dir-") as d:
        work=Path(d); binary=work/"dir-test"
        run(["cc","-std=c11","-O1","-g","-D_GNU_SOURCE","-fsanitize=address,undefined",
             "-fno-sanitize-recover=all","-ffunction-sections","-fdata-sections","-Wl,--gc-sections",
             "-DRELIEFOS_STORAGE_STANDALONE_TU","-Ikernel/reliefnt/include","-Iinclude",
             "-Ikernel/reliefnt/include/uapi","-Ikernel/reliefnt/kernel/reliefnt/include",
             "-include",str(STORAGE/"storage_internal.h"),"tools/tests/ext4_dir_test.c",
             *[str(FS/f"storage_ext4_{n}.c") for n in
               ("format","checksum","cache","alloc","extent","ops","xattr","journal","dir")],"-lext2fs","-o",str(binary)])
        stage=work/"stage"; (stage/"linux").mkdir(parents=True)
        for i in range(1000): (stage/"linux"/f"linux-file-{i:05d}").touch()
        for bs in (1024,4096):
            for kind,features in (("ext2","none,filetype,large_file,dir_index"),
                ("ext4","none,filetype,extents,64bit,flex_bg,metadata_csum,extra_isize,has_journal,dir_index")):
                image=work/f"{kind}-{bs}.img"; result=work/"result.img"
                with image.open("wb") as f: f.truncate(128*1024*1024)
                run(["mke2fs","-q","-F","-t",kind,"-b",str(bs),"-N","24000","-I","256","-O",features,
                     "-d",str(stage),str(image)])
                optimized=subprocess.run(["e2fsck","-f","-y","-D",str(image)],capture_output=True,text=True)
                if optimized.returncode not in (0,1): raise RuntimeError(optimized.stdout+optimized.stderr)
                run([str(binary),str(image),str(result),str(args.count)])
                run(["e2fsck","-f","-n",str(result)])
                print(f"PASS Linux directory oracle {kind} bs={bs}",flush=True)
    print("PASS ext4 directory suite")

if __name__=="__main__": main()
