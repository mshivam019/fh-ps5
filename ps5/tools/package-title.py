#!/usr/bin/env python3
"""Package the publishable app ZIP (PPSA99690/ at the top level): the runtime-only
eboot.bin, metadata, default configuration and license texts. No game data,
generated game code, saves or local configuration."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[2]
TITLE = 'PPSA99690'
SDK = ROOT / '.deps/rexglue-sdk'
VULKAN = ROOT.parent / 'PS5_Vulkan'
# License texts of everything linked into eboot.bin.
LICENSES = {
    'Forza-PS5-MIT.txt': ROOT / 'LICENSE',
    'ReXGlue-BSD-3-Clause.txt': SDK / 'LICENSE',
    'PS5-Vulkan-Template-MIT.md': ROOT / 'ps5/licenses/PS5-Vulkan-Template-MIT.md',
    'fmt.txt': SDK / 'thirdparty/fmt/LICENSE',
    'spdlog.txt': SDK / 'thirdparty/spdlog/LICENSE',
    'xxHash.txt': SDK / 'thirdparty/xxHash/LICENSE',
    'glslang.txt': SDK / 'thirdparty/glslang/LICENSE.txt',
    'libmspack-LGPL-2.1.txt': SDK / 'thirdparty/libmspack/libmspack/COPYING.LIB',
    'aes_128.txt': SDK / 'thirdparty/aes_128/LICENSE',
    'o1heap.txt': SDK / 'thirdparty/o1heap/LICENSE',
    'ImGui.txt': SDK / 'thirdparty/imgui/LICENSE.txt',
    'simde.txt': SDK / 'thirdparty/simde/COPYING',
    'tomlplusplus.txt': SDK / 'thirdparty/tomlplusplus/LICENSE',
    'VMA.txt': SDK / 'thirdparty/vulkan-memory-allocator/LICENSE.txt',
}
# Shared with the sibling PS5 ports that provide the same native dependencies.
SIBLING_LICENSES = ROOT.parent / 'Links-Awakening-PS5/ps5/licenses'
for name in ('FFmpeg-LGPL-2.1.txt', 'FFmpeg-LICENSE.md', 'LLVM.txt', 'Mesa.txt', 'PS5-SDK.txt',
             'PS5-Vulkan.txt', 'SDL.txt'):
    LICENSES[name] = SIBLING_LICENSES / name


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path, help='New ZIP path, e.g. release/PPSA99690.zip')
    args = parser.parse_args()
    if args.output.exists():
        parser.error('Output exists')
    files = {
        'eboot.bin': ROOT / 'build/native-title/eboot.bin',
        'sce_module/libc.prx': VULKAN / 'runtime/libc.prx',
        'sce_sys/param.json': ROOT / 'ps5/sce_sys/param.json',
        'sce_sys/icon0.png': ROOT / 'ps5/sce_sys/icon0.png',
        'sce_sys/pic0.png': ROOT / 'ps5/sce_sys/pic0.png',
        'sce_sys/pic1.png': ROOT / 'ps5/sce_sys/pic1.png',
        'forza.toml': ROOT / 'ps5/config/forza.toml',
        'SETUP.md': ROOT / 'ps5/SETUP.md',
        'THIRD_PARTY_NOTICES.txt': ROOT / 'ps5/THIRD_PARTY_NOTICES.txt',
    }
    files.update({'licenses/' + name: path for name, path in LICENSES.items()})
    missing = [name for name, path in files.items() if not path.is_file()]
    if missing:
        parser.error('Missing package inputs: ' + ', '.join(missing))
    if digest(files['sce_module/libc.prx']) != (VULKAN / 'runtime/libc.prx.sha256').read_text().split()[0]:
        parser.error('libc.prx does not match its recorded digest')
    # Only the module-loading app may be published; it carries no game code.
    if b'Game module missing' not in files['eboot.bin'].read_bytes():  # loader.cpp message
        parser.error('eboot.bin is not the module-loading app build')
    param = json.loads(files['sce_sys/param.json'].read_text())
    if param['titleId'] != TITLE:
        parser.error('param.json title mismatch')
    manifest = {'title_id': TITLE, 'content_version': param['contentVersion'], 'files': []}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, path in sorted(files.items()):
            archive.write(path, f'{TITLE}/{name}')
            manifest['files'].append({'path': name, 'bytes': path.stat().st_size, 'sha256': digest(path)})
        archive.writestr(f'{TITLE}/manifest.json', json.dumps(manifest, indent=2) + '\n')
    with zipfile.ZipFile(args.output) as archive:
        if archive.testzip():
            raise ValueError('ZIP CRC mismatch')
        names = archive.namelist()
        if any(not n.startswith(TITLE + '/') or '/game/' in n or '/user/' in n for n in names):
            raise ValueError('Unexpected path in app ZIP')
    print(f'{args.output}: {len(names)} files, sha256 {digest(args.output)}')


if __name__ == '__main__':
    main()
