#!/usr/bin/env python3
"""Archive the exact source of every component linked into the app's eboot.bin
(corresponding source for its GPL/LGPL parts). Adapted from the Naruto and Link's
Awakening PS5 ports. Contains no game data or generated game code."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SDK = ROOT / '.deps/rexglue-sdk'
# SDK submodules linked into the app.
SDK_PARTS = ['FFmpeg', 'libmspack', 'glslang', 'fmt', 'spdlog', 'xxHash', 'imgui', 'o1heap',
             'simde', 'tomlplusplus', 'vulkan-memory-allocator', 'vulkan-headers']


def git(repo, *args, binary=False):
    output = subprocess.check_output(['git', '-C', str(repo), *args])
    return output if binary else output.decode().strip()


def add_repository(output, manifest, name, repo):
    revision = git(repo, 'rev-parse', 'HEAD')
    changed = set(filter(None, git(repo, 'diff', '--name-only', '-z', 'HEAD', binary=True).decode().split('\0')))
    manifest['repositories'][name] = {'revision': revision, 'modified': bool(changed)}
    with tempfile.TemporaryFile() as stream:
        subprocess.run(['git', '-C', str(repo), 'archive', '--format=tar', f'--prefix={name}/', revision],
                       stdout=stream, check=True)
        stream.seek(0)
        with tarfile.open(fileobj=stream) as source:
            for member in source:
                if member.name.removeprefix(name + '/') in changed:
                    continue
                output.addfile(member, source.extractfile(member) if member.isfile() else None)
    if changed:
        patch = git(repo, 'diff', '--binary', 'HEAD', binary=True)
        info = tarfile.TarInfo(name + '/LOCAL-BUILD.patch')
        info.size = len(patch)
        output.addfile(info, io.BytesIO(patch))
        for relative in changed:
            path = repo / relative
            if path.is_file():
                output.add(path, arcname=f'{name}/{relative}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('Output exists; choose a new artifact')
    workspace = ROOT.parent
    repositories = {
        'forza-ps5': ROOT,
        'rexglue-sdk': SDK,
        **{f'rexglue-sdk-thirdparty/{part}': SDK / 'thirdparty' / part for part in SDK_PARTS},
        'ps5-payload-sdk-platform': workspace / 'PS5_PayloadSDK',
        'ps5-vulkan': workspace / 'PS5_Vulkan',
        'mesa': workspace / 'Dusklight-PS5/.deps/src/mesa',
        'SDL': workspace / 'Dusklight-PS5/.deps/src/SDL',
        'native-app-boilerplate': Path.home() / 'ps5sdk/native-app-boilerplate',
    }
    manifest = {'repositories': {}, 'files': []}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(args.output.suffix + '.part')
    try:
        with tarfile.open(temporary, 'w:gz') as output:
            for name, repo in repositories.items():
                if not (repo / '.git').exists():
                    raise ValueError(f'Missing source repository: {repo}')
                add_repository(output, manifest, name, repo)
            provenance = workspace / 'PS5_Vulkan/.deps/native/radv-release/PROVENANCE.txt'
            if provenance.is_file():
                output.add(provenance, arcname='RADV-PROVENANCE.txt')
        with tarfile.open(temporary) as archive:
            for member in archive:
                if member.isfile():
                    data = archive.extractfile(member).read()
                    manifest['files'].append({'path': member.name, 'bytes': len(data),
                                              'sha256': hashlib.sha256(data).hexdigest()})
        temporary.replace(args.output)
    finally:
        temporary.unlink(missing_ok=True)
    manifest_path = args.output.with_name(args.output.name + '.manifest.json')
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'{args.output}: {len(manifest["files"])} files from {len(repositories)} repositories')


if __name__ == '__main__':
    main()
