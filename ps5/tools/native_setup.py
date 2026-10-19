# SPDX-License-Identifier: MIT
"""Native Windows desktop tools; the PS5 compilation target stays unchanged."""
import json
import os
from pathlib import Path
import subprocess
import zipfile


def executable(directory, name):
    candidate = directory / (name + '.exe' if os.name == 'nt' else name)
    if not candidate.is_file():
        raise ValueError('Missing tool: ' + str(candidate))
    return candidate


def unpack_zip(archive, destination):
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            name = info.filename.replace('\\', '/')
            if name.startswith('/') or ':' in name or '..' in name.split('/') or (info.external_attr >> 16) & 0o170000 == 0o120000:
                raise ValueError('Unsafe tool archive entry: ' + info.filename)
        z.extractall(destination)


def check_windows_runtime():
    if os.name != 'nt':
        return
    import ctypes
    missing = []
    for name in ('msvcp140.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll'):
        try:
            ctypes.WinDLL(name)
        except OSError:
            missing.append(name)
    if missing:
        raise ValueError('Install Microsoft Visual C++ Redistributable x64, then restart setup: '
                         'https://aka.ms/vc14/vc_redist.x64.exe (missing: ' + ', '.join(missing) + ')')


def windows_tools(root, cache, download):
    pins = json.loads((root / 'ps5/tools/windows-tools.json').read_text())
    cache.mkdir(parents=True, exist_ok=True)
    found = []
    for key, filename in [('codegen', 'rexglue.exe'), ('compiler', 'clang++.exe')]:
        pin = pins[key]
        archive = download(pin, cache)
        directory = cache / key
        stamp = directory / '.archive-sha256'
        target = directory / pin['directory'] / 'bin' / filename
        if not (target.is_file() and stamp.is_file() and stamp.read_text() == pin['sha256']):
            directory.mkdir(exist_ok=True)
            unpack_zip(archive, directory)
            stamp.write_text(pin['sha256'])
        found.append(target if key == 'codegen' else target.parent)
    return tuple(found)


def fetch_forza(root, pins, run):
    """Python equivalent of source-only SDK preparation, with no Bash/reset/clean."""
    source = root / 'upstream/forza'
    sdk = root / '.deps/rexglue-sdk'
    for path, pin in [(source, pins['forza']), (sdk, pins['rexglue_sdk'])]:
        if not path.exists():
            path.parent.mkdir(parents=True, exist_ok=True)
            run('git', '-c', 'core.autocrlf=false', 'clone', '--no-checkout', pin['url'], path)
            run('git', '-C', path, 'config', 'core.autocrlf', 'false')
            run('git', '-C', path, 'checkout', '--detach', pin['revision'])
        actual = subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()
        if actual != pin['revision']:
            raise ValueError('Unexpected source revision at ' + str(path))
    ps5_patch = root / 'ps5/patches/rexglue-ps5.patch'
    if subprocess.run(['git', '-C', str(sdk), 'apply', '--reverse', '--check', str(ps5_patch)],
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
        return source, sdk
    if subprocess.check_output(['git', '-C', str(sdk), 'status', '--porcelain', '--untracked-files=no']).strip():
        raise ValueError('SDK has unfinished edits; use a fresh extracted setup folder: ' + str(sdk))
    run('git', '-c', 'core.autocrlf=false', '-C', sdk, 'submodule', 'update', '--init', '--recursive', '--depth', '1')
    for patch in sorted((source / 'patches/rexglue-sdk').glob('*.patch')):
        run('git', '-C', sdk, 'apply', patch)
    run('git', '-C', sdk, 'apply', ps5_patch)
    return source, sdk


def response_file(path, arguments):
    """LLVM accepts quoted UTF-8 response files, avoiding Windows command limits."""
    lines = []
    for value in arguments:
        value = str(value)
        if any(c in value for c in '\r\n\0'):
            raise ValueError('Invalid newline/NUL in compiler argument')
        lines.append('"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"')
    path.write_text('\n'.join(lines) + '\n', encoding='utf-8', newline='\n')
    return '@' + str(path)
