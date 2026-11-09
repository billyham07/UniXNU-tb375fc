#!/usr/bin/env python3
"""Build libTb375fcStubs.a with the XNU build's own compile record (method of UniXNU-uke build-boot-log.py).

Members: Tb375fcStubs.c (weak hooks of the sheng 27 patches), Tb375fcDiag.c (mrdump recorder) and
Tb375fcDiagGlue.c (boot_progress hook points -> recorder, patches/xnu-12377/tb375fc-diag-hooks.patch).
"""
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/tb375fc-stubs'
OUT.mkdir(parents=True, exist_ok=True)
RECORD = 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json'
KIT = ROOT / 'kits/tb375fc'
SOURCES = ['Tb375fcStubs.c', 'Tb375fcDiag.c', 'Tb375fcDiagGlue.c']

record = json.loads((ROOT / RECORD).read_text().strip().rstrip(','))
objects = []
for name in SOURCES:
    source, obj = KIT / name, OUT / (Path(name).stem + '.o')
    command, args, i = [], record['arguments'], 0
    while i < len(args):
        arg = args[i]
        if arg == record['file']:
            command.append(str(source))
        elif arg in ('-o', '-MF', '-MT', '-MJ'):
            if arg == '-o':
                command += ['-o', str(obj)]
            i += 1
        elif arg == '-Xclang' and args[i + 1].startswith('-W'):
            i += 1
        elif not arg.startswith('-W'):
            command.append(arg)
        i += 1
    command += ['-I', str(KIT), '-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror']
    subprocess.run(command, cwd=record['directory'], check=True, timeout=120)
    objects.append(str(obj))
archive = OUT / 'libTb375fcStubs.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *objects], check=True, timeout=30)
print('PASS: libTb375fcStubs.a', ' '.join(SOURCES))
