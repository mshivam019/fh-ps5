#!/usr/bin/env python3
"""Package the desktop setup: this repository's committed files. Setup fetches
the pinned public sources and tools itself; no game data is included."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[2]
BANNED_PARTS = {'private', 'build', 'dist', 'evidence', '.deps', 'upstream', '.git'}
BANNED_SUFFIXES = {'.iso', '.xex', '.dll', '.module', '.bin', '.7z'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('Output exists')
    if subprocess.check_output(['git', '-C', ROOT, 'status', '--porcelain', '--untracked-files=no']).strip():
        parser.error('Commit the source before packaging')
    names = [n for n in subprocess.check_output(['git', '-C', ROOT, 'ls-files', '-z']).decode().split('\0') if n]
    prefix = args.output.stem
    manifest = []
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name in sorted(names):
            path = ROOT / name
            relative = Path(name)
            if path.is_symlink() or any(p in BANNED_PARTS for p in relative.parts) or \
                    relative.suffix.lower() in BANNED_SUFFIXES:
                raise ValueError(f'Refusing to package {name}')
            data = path.read_bytes()
            info = zipfile.ZipInfo(f'{prefix}/{name}', date_time=(2026, 1, 1, 0, 0, 0))
            info.external_attr = (path.stat().st_mode & 0o777) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data)
            manifest.append({'path': name, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
        archive.writestr(f'{prefix}/setup-manifest.json', json.dumps({'files': manifest}, indent=2) + '\n')
    with zipfile.ZipFile(args.output) as archive:
        if archive.testzip():
            raise ValueError('ZIP CRC mismatch')
    print(f'{args.output}: {len(manifest)} files')


if __name__ == '__main__':
    main()
