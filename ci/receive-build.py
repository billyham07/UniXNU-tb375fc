#!/usr/bin/env python3
"""Download and decrypt an explicitly approved successful CI commit. Never flashes a device."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=int)
    parser.add_argument('commit', help='Full reviewed device-source SHA, not a branch name')
    parser.add_argument('output', type=Path, help='Must be a new directory')
    parser.add_argument('--key', required=True, type=Path, help='Operator private key, kept off GitHub')
    parser.add_argument('--repo', default='billyham07/UniXNU-tb375fc')
    args = parser.parse_args()
    if len(args.commit) != 40 or any(c not in '0123456789abcdef' for c in args.commit):
        parser.error('commit must be a full lowercase SHA')
    run = json.loads(subprocess.check_output(
        ['gh', 'api', f'repos/{args.repo}/actions/runs/{args.run}'], text=True))
    if (run['head_sha'] != args.commit or run['conclusion'] != 'success' or
            run['event'] != 'workflow_dispatch' or run['path'] != '.github/workflows/macos.yml'):
        parser.error('run does not match the approved successful build')
    certificate = Path(__file__).parent / 'recipient.pem'
    if subprocess.check_output(['git', '-C', str(Path(__file__).parent), 'rev-parse', 'HEAD'],
                               text=True).strip() != args.commit:
        parser.error('check out the reviewed commit before receiving its build')
    args.output.mkdir(parents=True, exist_ok=False)
    encrypted = args.output / 'encrypted'
    attempt = str(run['run_attempt'])
    subprocess.run(['gh', 'run', 'download', str(args.run), '--repo', args.repo,
                    '--name', f'kernel-encrypted-{args.run}-{attempt}',
                    '--dir', str(encrypted)], check=True)
    manifest = json.loads((encrypted / 'manifest.json').read_text())
    identity = dict(GITHUB_REPOSITORY=args.repo, GITHUB_SHA=args.commit,
                    GITHUB_RUN_ID=str(args.run), GITHUB_RUN_ATTEMPT=attempt)
    if any(manifest.get(k) != v for k, v in identity.items()):
        parser.error('artifact identity does not match the requested run')
    ciphertext = encrypted / 'kernel.tar.gz.cms'
    if (sha(ciphertext) != manifest['ciphertext_sha256'] or
            sha(certificate) != manifest['certificate_sha256']):
        parser.error('ciphertext or recipient certificate hash mismatch')
    bundle = args.output / 'kernel.tar.gz'
    subprocess.run(['openssl', 'cms', '-decrypt', '-binary', '-inform', 'DER',
                    '-in', str(ciphertext), '-recip', str(certificate),
                    '-inkey', str(args.key.resolve()), '-out', str(bundle)], check=True)
    try:
        with tarfile.open(bundle) as archive:
            members = archive.getmembers()
            names = [m.name for m in members]
            if (len(names) != len(set(names)) or sum(m.size for m in members) > 1 << 30 or
                    any(not m.isfile() or
                        not (m.name == 'build-info.json' or
                             (len(Path(m.name).parts) == 2 and Path(m.name).parts[0] == 'kernel' and
                              Path(m.name).suffix in ('.macho', '.json')))
                        for m in members)):
                parser.error('unexpected archive member')
            archive.extractall(args.output, filter='data')
        info = json.loads((args.output / 'build-info.json').read_text())
        if any(info.get(k) != v for k, v in identity.items()):
            parser.error('encrypted manifest identity mismatch')
        files = sorted((args.output / 'kernel').iterdir())
        if {p.name: sha(p) for p in files} != info['files'] or not (args.output / 'kernel/kernel.macho').is_file():
            parser.error('decrypted kernel digest mismatch')
    finally:
        bundle.unlink(missing_ok=True)
    print('Verified and decrypted:', args.output / 'kernel/kernel.macho')
    print('Build only: not installed, flashed or hardware-qualified')


if __name__ == '__main__':
    main()
