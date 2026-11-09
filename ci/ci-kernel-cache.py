#!/usr/bin/env python3
"""Key and validate the macOS runner's reusable XNU build outputs (ported from the private build repo).

The exact key changes with every checkout, so updated reference outputs can be
saved under a new immutable Actions cache entry.  Its stable prefix covers the
inputs that require a fresh bootstrap pass.  A package or auxiliary-script
change can therefore restore compatible compile records without incorrectly
reusing the RELEASE reference: that builder has its own content signature.
"""
import argparse
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

# TB375FC public CI: device/ and the pinned UniXNU framework/ are sibling checkouts; run from framework/.
ROOT = Path(os.environ.get('UNIXNU_ROOT', Path.cwd())).resolve()
sys.path.insert(0, str(ROOT / 'scripts'))

from unixnu.cli import cmd_status, parser as unixnu_parser  # noqa: E402
from unixnu.config import Context  # noqa: E402


PAYLOAD_ONLY = ('adt', 'payload', 'userdata', 'root', 'device_tree', 'firmware')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_digest(path):
    return digest(Path(path).read_bytes())


def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args])


def resolved_sources(status):
    """Source identity independent of whether a Git feed is already downloaded."""
    result = []
    for module in status['modules']:
        item = {key: module.get(key) for key in ('kind', 'name', 'origin', 'repository', 'pin')}
        path = Path(module['path']) if module.get('path') else None
        if module['origin'] == 'git':
            # A pinned commit is immutable: before and after fetch must hash alike.
            item['revision'] = module.get('pin') or module.get('revision')
            if path and not module.get('pin'):
                # An unpinned branch must be sampled once its checkout is available.
                item['path'] = str(path)
                item['tree'] = git(path, 'rev-parse', 'HEAD^{tree}').decode().strip()
        else:
            item['path'] = module.get('path')
            # In-tree modules share the parent commit, but package/script edits
            # need only an exact-cache miss. A genuinely local XNU is different.
            item['revision'] = module.get('pin')
            if module['kind'] == 'refer' and module['name'] == 'xnu':
                item['revision'] = module.get('revision')
        result.append(item)
    return result


def repository_snapshot(root):
    """Committed state plus any tracked/untracked edits made by pre_build."""
    untracked = []
    listing = git(root, 'ls-files', '--others', '--exclude-standard', '-z')
    for raw in sorted(filter(None, listing.split(b'\0'))):
        relative = raw.decode()
        # defconfig and the key command create output state and bytecode, not inputs.
        if relative == 'out' or relative.startswith('out/') or '__pycache__' in Path(relative).parts:
            continue
        path = root / relative
        # actions/checkout uses an embedded device checkout; a local device
        # checkout is instead a Git worktree with .git as a file.
        if relative.rstrip('/') == 'device' and (path / '.git').exists():
            continue
        # Empty directories left by checkout for uninitialised submodules are
        # not source changes. Initialised submodules have their own .git and
        # are covered by their recorded pins, not by a recursive hash here.
        if path.is_dir() and any(item.is_dir() and (item / '.git').exists() for item in path.iterdir()):
            continue
        if not path.is_file():
            raise RuntimeError(f'cannot fingerprint non-regular untracked input: {path}')
        untracked.append({'path': relative, 'sha256': file_digest(path)})
    return {
        'head': git(root, 'rev-parse', 'HEAD').decode().strip(),
        'diff_sha256': digest(git(root, 'diff', '--binary', '--no-ext-diff', 'HEAD', '--')),
        'untracked': untracked,
    }


def command_output(*args):
    try:
        return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT).strip()
    except (OSError, subprocess.CalledProcessError) as error:
        # The key command is also testable on a Linux porting host.  The macOS
        # workflow checks the real toolchain before reaching this step.
        return f'unavailable: {type(error).__name__}'


def make_key():
    ctx = Context(unixnu_parser().parse_args(['status', '--json']))
    output = io.StringIO()
    with contextlib.redirect_stdout(output):
        cmd_status(ctx)
    status = json.loads(output.getvalue())
    patches = []
    for name in status['patch_order']:
        path = Path(name)
        patches.append({'path': name, 'sha256': file_digest(path) if path.is_file() else None})
    builders = {}
    for name in ('scripts/unixnu/kernel.py', 'scripts/unixnu/patches.py',
                 'scripts/build-xnu-4k-baseline.sh', 'scripts/prepare-xnu-source.py'):
        builders[name] = file_digest(ROOT / name)
    base = {
        'schema': 5,
        # Payload/ADT/firmware tables do not reach the XNU build; editing them must not force a cold bootstrap.
        'board': {k: v for k, v in status['board'].items() if k not in PAYLOAD_ONLY},
        'components': status['components'],
        'config': file_digest(ctx.config_path),
        'patches': patches,
        'sources': resolved_sources(status),
        'builders': builders,
        'toolchain': {
            'runner_image': os.environ.get('ImageOS', ''),
            'macos': command_output('sw_vers'),
            'xcode': command_output('xcodebuild', '-version'),
            'clang': command_output('xcrun', 'clang', '--version'),
        },
        'pre_build': os.environ.get('PRE_BUILD', ''),
        'cache_salt': os.environ.get('CACHE_SALT', ''),
    }
    repositories = [ROOT, ctx.device]
    if ctx.soc and ctx.soc.path:
        soc_root = Path(ctx.soc.path)
        # A device's soc/ overlay is part of the device checkout, not an extra repository.
        if not soc_root.is_relative_to(ctx.device):
            repositories.append(soc_root)
    # The key runs before submodule update; uninitialized placeholders are not inputs.
    submodule_paths = set()
    for repository in repositories:
        result = subprocess.run(['git', '-C', str(repository), 'submodule', 'status'],
                                capture_output=True, text=True)
        if result.returncode == 0:
            for line in result.stdout.splitlines():
                fields = line.split()
                if len(fields) > 1:
                    submodule_paths.add(str((repository / fields[1]).resolve()))
    status['modules'] = [module for module in status['modules']
                         if not module.get('path') or str(Path(module['path']).resolve()) not in submodule_paths]
    exact = {
        'base': base,
        'status': status,
        'repositories': {str(path): repository_snapshot(path) for path in repositories},
        'sources': resolved_sources(status),
    }
    base_digest = digest(json.dumps(base, sort_keys=True, separators=(',', ':')).encode())
    exact_digest = digest(json.dumps(exact, sort_keys=True, separators=(',', ':')).encode())
    prefix = f'tb375fc-xnu-v4-{base_digest}'
    ipsw = dict(status['board']['ipsw'])
    ipsw['kernelcache_sha256'] = status['board']['kernel']['prelink'].get('kernelcache_sha256')
    build = ipsw['build']
    values = {
        'cache-key': f'{prefix}-{exact_digest}',
        'restore-key': f'{prefix}-',
        'ipsw-cache-key': f'tb375fc-ipsw-v1-{build}-{digest(json.dumps(ipsw, sort_keys=True).encode())}',
        'ipsw-pin': json.dumps(ipsw, separators=(',', ':')),
    }
    with open(os.environ['GITHUB_OUTPUT'], 'a') as stream:
        for name, value in values.items():
            stream.write(f'{name}={value}\n')
    print(f"kernel cache base {base_digest}; exact {exact_digest}; work {ctx.work}")


def validate(work, pin):
    """Reject a partial/mismatched restore before make can consume its objects."""
    work = Path(work)
    tree = work / 'tree'
    source = tree / '.cache/xnu-work'
    stamp = work / 'xnu-inputs.json'
    if source.is_symlink():
        raise SystemExit(f'kernel cache: refusing to validate a symlinked XNU tree: {source}')
    if not stamp.exists() and not source.exists():
        print('kernel cache: cold build (no patched source or input stamp)')
        return

    def invalidate(reason):
        print(f'kernel cache: discarding inconsistent restore: {reason}')
        shutil.rmtree(source, ignore_errors=True)
        stamp.unlink(missing_ok=True)
        shutil.rmtree(tree / 'build/xnu/ios-obj', ignore_errors=True)
        reference = tree / 'build/xnu/fileset-reference/mach.release.bcm2837.unstripped.noctf'
        reference.unlink(missing_ok=True)
        reference.with_suffix('.json').unlink(missing_ok=True)

    try:
        saved = json.loads(stamp.read_text())
        revision = git(source, 'rev-parse', 'HEAD').decode().strip()
        if revision != saved['commit']:
            raise ValueError(f"XNU revision {revision} does not match stamp {saved['commit']}")
        for patch in saved['patches']:
            if file_digest(patch['path']) != patch['sha256']:
                raise ValueError(f"patch changed: {patch['path']}")
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        invalidate(str(error))
        return
    print(f"kernel cache: validated XNU {revision[:12]} and {len(saved['patches'])} patches")
    record = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json'
    if not record.is_file():
        return
    newest_source = max(path.stat().st_mtime_ns for path in source.rglob('*')
                        if path.is_file() and '.git' not in path.parts)
    newest_object = 0
    for path in (tree / 'build/xnu/ios-obj').rglob('*.o'):
        if path.is_file():
            newest_object = max(newest_object, path.stat().st_mtime_ns)
    if newest_object < newest_source:
        # A cache extractor can preserve sources with newer checkout mtimes.
        # Rebuild once, rather than silently accepting a stale object graph.
        invalidate(f'object timestamps precede restored sources: {newest_object} < {newest_source}')
        return
    kernelcache = work.parents[2] / 'ipsw' / pin['build'] / 'kernelcache.decompressed'
    wanted = pin.get('kernelcache_sha256')
    if kernelcache.is_file():
        actual = file_digest(kernelcache)
        if wanted and actual != wanted:
            kernelcache.unlink()
            print('kernel cache: removed a kernelcache with the wrong pinned SHA256')
        else:
            print(f'kernel cache: validated kernelcache {actual[:12]}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    sub.add_parser('key')
    check = sub.add_parser('validate')
    check.add_argument('--work', required=True, type=Path)
    check.add_argument('--ipsw-pin', required=True, help='JSON [ipsw] table and kernel.prelink SHA256 pin')
    args = parser.parse_args()
    if args.action == 'key':
        make_key()
    else:
        validate(args.work, json.loads(args.ipsw_pin))


if __name__ == '__main__':
    main()
