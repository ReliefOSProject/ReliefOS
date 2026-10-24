#!/usr/bin/env python3
"""Run native application C/C++ actions with real compilers and ELF outputs."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(args, cwd):
    result = subprocess.run(args, cwd=cwd, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=180)
    assert result.returncode == 0, result.stdout
    return result.stdout


def main():
    temporary = Path(os.environ.get('TMPDIR', str(ROOT / 'out/tmp'))).resolve()
    temporary.mkdir(parents=True, exist_ok=True)
    cc = shutil.which('clang') or shutil.which('cc')
    cxx = shutil.which('clang++') or shutil.which('c++')
    ar = shutil.which('llvm-ar') or shutil.which('ar')
    assert cc and cxx and ar
    with tempfile.TemporaryDirectory(prefix='userland-kbuild-', dir=temporary) as directory:
        work = Path(directory)
        src, out = work / 'src', work / 'out'
        src.mkdir()
        out.mkdir()
        (src / 'scripts').symlink_to(ROOT / 'scripts', target_is_directory=True)
        (src / 'config.h').write_text('#define FIXTURE_VALUE 7\n')
        (src / 'api.h').write_text('#define FIXTURE_HEADER 1\n')
        for name in ('one', 'two'):
            app = src / 'userland/apps' / name
            app.mkdir(parents=True)
            (app / 'main.cpp').write_text(
                '#include "api.h"\nextern "C" int answer(void);\n'
                'int main() { return answer() == FIXTURE_VALUE + FIXTURE_HEADER ? 0 : 1; }\n')
            (app / 'answer.c').write_text(
                '#include "api.h"\nint answer(void) { return FIXTURE_VALUE + FIXTURE_HEADER; }\n')
            (app / 'extra.c').write_text('int extra_member(void) { return 12; }\n')
            (app / 'Kbuild').write_text(
                f'RELIEFOS_APP_NAME := {name}\n'
                'RELIEFOS_APP_OBJECTS := answer.o extra.o main.o\n'
                'RELIEFOS_APP_C_OBJECTS := answer.o extra.o\n'
                'RELIEFOS_APP_CXX_OBJECTS := main.o\n'
                'RELIEFOS_APP_ASM_OBJECTS :=\n'
                'include $(srctree)/scripts/reliefos/Makefile.app-kbuild\n')
        (src / 'Kbuild').write_text('''obj-y := userland/apps/one/ userland/apps/two/
export USERLAND_FLAGS USERLAND_CXX_FLAGS TARGET_CXX AUTOCONF_H
USERLAND_FLAGS := -O2 -std=c11 -ffreestanding -I$(srctree)
USERLAND_CXX_FLAGS := -O2 -std=c++17 -nostdinc++ -fno-exceptions -fno-rtti -I$(srctree) $(CXX_TOGGLE)
AUTOCONF_H := $(srctree)/config.h
quiet_cmd_fixture_link = LD $@
      cmd_fixture_link = $(TARGET_CXX) -o $@ -Wl,--whole-archive userland/apps/$@/built-in.a -Wl,--no-whole-archive
.PHONY: apps
apps: one two
one: userland/apps/one/built-in.a FORCE
\t$(call if_changed,fixture_link)
two: userland/apps/two/built-in.a FORCE
\t$(call if_changed,fixture_link)
targets += one two
''')
        base = ['make', '--no-print-directory', '-rR', '-j2', '-f',
                str(ROOT / 'scripts/Makefile.port'), '-f', str(ROOT / 'scripts/Makefile.build'),
                f'srctree={src}', f'objtree={out}', f'VPATH={src}',
                f'CC={cc}', f'TARGET_CXX={cxx}', f'AR={ar}', f'HOSTCC={cc}',
                'KBUILD_BUILTIN=1', 'need-builtin=1', 'CONFIG_SHELL=/bin/sh', 'AWK=awk',
                'RELIEFOS_CC_ID=', 'RELIEFOS_CXX_ID=', 'RELIEFOS_LD_ID=', 'RELIEFOS_AR_ID=',
                f'KBUILD_HOSTCFLAGS=-O2 -I{ROOT}/scripts/include']
        run(base + ['obj=scripts/basic'], out)

        def build(toggle=''):
            return run(base + ['obj=.', f'CXX_TOGGLE={toggle}', 'apps'], out)

        def stamps():
            return {str(p.relative_to(out)): p.stat().st_mtime_ns for p in out.rglob('*.o')}

        build()
        for name in ('one', 'two'):
            run([str(out / name)], out)
            command = (out / f'userland/apps/{name}/.main.o.cmd').read_text()
            assert '-std=c++17' in command and '-nostdinc++' in command
            assert '-ffreestanding' not in command and '-std=c11' not in command
            assert 'api.h' in command, 'C++ fixdep lost a real include'
        original = stamps()
        binaries = {n: (out / n).stat().st_mtime_ns for n in ('one', 'two')}
        assert not build().strip(), 'native application no-op ran an action'
        assert stamps() == original
        assert binaries == {n: (out / n).stat().st_mtime_ns for n in binaries}
        build('-DFIXTURE_CXX_TOGGLE=1')
        changed = {n for n, stamp in stamps().items() if original[n] != stamp}
        assert changed == {'userland/apps/one/main.o', 'userland/apps/two/main.o'}, changed
        original = stamps()
        (src / 'api.h').write_text('#define FIXTURE_HEADER 2\n')
        build('-DFIXTURE_CXX_TOGGLE=1')
        changed = {n for n, stamp in stamps().items() if original[n] != stamp}
        assert changed == {f'userland/apps/{n}/{o}.o' for n in ('one', 'two') for o in ('answer', 'main')}, changed
        declaration = src / 'userland/apps/one/Kbuild'
        declaration.write_text(declaration.read_text().replace('extra.o ', ''))
        (src / 'userland/apps/one/extra.c').unlink()
        build('-DFIXTURE_CXX_TOGGLE=1')
        members = run([ar, 't', str(out / 'userland/apps/one/built-in.a')], out)
        symbols = run(['nm', str(out / 'one')], out)
        assert 'extra.o' not in members and 'extra_member' not in symbols
        assert 'extra_member' in run(['nm', str(out / 'two')], out)
        original = stamps()
        (out / 'one').unlink()
        build('-DFIXTURE_CXX_TOGGLE=1')
        assert stamps() == original, 'missing executable repair needlessly recompiled sources'
        run([str(out / 'one')], out)
        (out / 'userland/apps/one/main.o').unlink()
        build('-DFIXTURE_CXX_TOGGLE=1')
        run([str(out / 'one')], out)
        assert not any(src.rglob('*.o')), 'application build wrote source-tree objects'

        # Exercise the actual production component pruning rule, retaining
        # selected applications and unknown user files in the same directory.
        product = work / 'prune'
        make = ['make', '--no-print-directory', '-s', '-C', str(ROOT), f'O={product}']
        run(make + ['defconfig'], ROOT)
        config = product / '.config'
        text = config.read_text()
        key = 'CONFIG_RELIEFOS_COMPONENT_APP_CALC_BUILD=y'
        assert key in text
        config.write_text(text.replace(key, '# CONFIG_RELIEFOS_COMPONENT_APP_CALC_BUILD is not set'))
        run(make + ['olddefconfig'], ROOT)
        run(make + ['config-sync'], ROOT)
        userland = product / 'userland'
        userland.mkdir(exist_ok=True)
        for name in ('calc.elf', 'hello.elf', 'user-owned.elf'):
            (userland / name).write_text('owned fixture\n')
        run(make + ['userland-prune'], ROOT)
        assert not (userland / 'calc.elf').exists(), 'disabled application was retained'
        assert (userland / 'hello.elf').exists(), 'selected application was removed'
        assert (userland / 'user-owned.elf').exists(), 'unknown user file was removed'
    print('PASS native application C/C++ flags, fixdep, no-op, member removal, repair, isolation and component pruning')


if __name__ == '__main__':
    main()
