#!/usr/bin/env python3
"""Seal mixed kernel output for one recipient; never publish the plaintext."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernel', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('certificate', type=Path)
    args = parser.parse_args()
    if not (args.kernel / 'kernel.macho').is_file():
        parser.error('the real kernel output is missing')
    args.output.mkdir(parents=True, exist_ok=False)
    identity = {name: os.environ[name] for name in
                ('GITHUB_REPOSITORY', 'GITHUB_SHA', 'GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT')}
    identity['framework_revision'] = (Path(__file__).parent / 'framework-revision').read_text().strip()
    files = sorted(args.kernel.iterdir())
    if any(p.is_symlink() or not p.is_file() or p.suffix not in ('.macho', '.json') for p in files):
        parser.error('unexpected entry in the kernel directory')
    details = dict(identity, files={p.name: sha(p) for p in files})
    ciphertext = args.output / 'kernel.tar.gz.cms'
    with tempfile.TemporaryDirectory(prefix='tb375fc-seal-') as temporary:
        temporary = Path(temporary)
        info = temporary / 'build-info.json'
        info.write_text(json.dumps(details, indent=2) + '\n')
        bundle = temporary / 'kernel.tar.gz'
        with tarfile.open(bundle, 'w:gz') as archive:
            archive.add(info, arcname='build-info.json')
            for path in files:
                archive.add(path, arcname='kernel/' + path.name, recursive=False)
        subprocess.run([os.environ.get('OPENSSL', 'openssl'), 'cms', '-encrypt', '-binary',
                        '-aes-256-gcm', '-in', str(bundle), '-out', str(ciphertext),
                        '-outform', 'DER', '-recip', str(args.certificate),
                        '-keyopt', 'rsa_padding_mode:oaep', '-keyopt', 'rsa_oaep_md:sha256'], check=True)
    manifest = dict(identity, ciphertext_sha256=sha(ciphertext),
                    certificate_sha256=sha(args.certificate),
                    encryption='CMS AuthEnvelopedData / AES-256-GCM / RSA-OAEP-SHA256')
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('Sealed kernel for the checked-in recipient; no private key used')


if __name__ == '__main__':
    main()
