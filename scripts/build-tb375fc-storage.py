#!/usr/bin/env python3
"""Build the TB375FC read-only root adapter against the pinned IOStorage API.

Late base-class registration and constructor extraction follow UniXNU's
scripts/build-sheng-source-storage.py. Original IOStorageFamily/APFS stay in
kernelcache; no Qualcomm storage, VerifiedRoot policy or Data writer is linked.
"""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/tb375fc-storage'
OUT.mkdir(parents=True, exist_ok=True)


def helper(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


headers = helper('tb_storage_headers', ROOT / 'scripts/prepare-storage-headers.py')
build = helper('tb_storage_compile', ROOT / 'scripts/build-sheng-source-drivers.py')
source = ROOT / 'refer/IOStorageFamily'
header_report = headers.prepare(source, '7edb88fbae296fb7c8ce2f64e115e116e566d51c', OUT / 'headers')
base = OUT / 'IOBlockStorageDevice.cpp'
original = (source / 'IOBlockStorageDevice.cpp').read_text()
macro = 'OSDefineMetaClassAndAbstractStructors(IOBlockStorageDevice,IOService)'
assert original.count(macro) == 1, 'pinned IOBlockStorageDevice constructor changed'
base.write_text(original.replace(macro, '''
extern "C" const OSMetaClass *tb375fc_storage_parent(void);
OSDefineMetaClassWithInit(IOBlockStorageDevice, IOService, )
IOBlockStorageDevice::MetaClass::MetaClass()
    : OSMetaClass("Tb375fcBlockStorageDevice", tb375fc_storage_parent(), sizeof(IOBlockStorageDevice)) {}
OSDefineAbstractStructors(IOBlockStorageDevice, IOService)
OSDefineOperatorMethods(IOBlockStorageDevice)
'''))
record_path = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
record = json.loads(record_path.read_text().strip().rstrip(','))
kit = ROOT / 'kits/tb375fc'
includes = ['-I', str(OUT / 'headers/include'), '-I', str(kit)]
objcopy = shutil.which('llvm-objcopy') or '/opt/homebrew/opt/llvm/bin/llvm-objcopy'
objects = []
for path in [base, kit / 'Tb375fcUfs.cpp']:
    obj = OUT / (path.stem + '.o')
    flags = ['-Wno-error'] if path == base else []
    command = build.compile_command(record, path, obj, includes, flags)
    (OUT / (path.stem + '.command.json')).write_text(json.dumps(command, indent=2))
    result = subprocess.run(command, cwd=record['directory'], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (OUT / (path.stem + '.log')).write_text(result.stdout)
    if result.returncode:
        raise SystemExit(result.stdout[-10000:])
    late = obj.with_suffix('.late.o')
    subprocess.run([objcopy, '--remove-section=__DATA,__mod_init_func',
                    f'--globalize-symbol=__GLOBAL__sub_I_{path.name}', str(obj), str(late)], check=True)
    objects.append(str(late))
archive = OUT / 'libTb375fcStorage.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *objects], check=True)
(OUT / 'report.json').write_text(json.dumps({'archive': str(archive), 'headers': header_report,
    'reference': 'UniXNU/scripts/build-sheng-source-storage.py; UniXNU-sheng/package/SourceStorage.cpp',
    'readonly': True}, indent=2))
print('PASS:', archive)
