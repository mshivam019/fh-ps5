#!/usr/bin/env python3
"""Prepare your own Forza Horizon disc for the PS5 app.

Extracts the disc image, generates and compiles the game code into the private
game module, and writes a PPSA99690/game folder to copy into the installed app.
Linux x86-64 or native Windows x64 (Python 3.11+ and Git). The output contains your game data
and code generated from it: never share it."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import tomllib
import urllib.request
import zipfile

from ps5_x360 import finish_setup, interactive_build
from native_setup import windows_tools, unpack_zip, fetch_forza, check_windows_runtime

ROOT = Path(__file__).resolve().parents[2]
PINS = json.loads((ROOT / 'sources.json').read_text())
TOOLS = json.loads((ROOT / 'ps5/module/toolchain.json').read_text())
# The supported disc: Forza Horizon (USA) disc 1, base version, title 4D5309C9.
GAME_FILES = {
    'default.xex': 'db40df605ade49a612b35a7a24c38f6004bcb17a88ed6b48288de16df9e3987c',
    'xmediafacade_default.xex': 'ed285beefeafcdc90cf64d6085d19c874e3f1e15072c12cdf9ba2237c88e2ce5',
    'speechfacade_default.xex': 'd54f6aae6ae2fd6da9b8c37d48f0a6bb295caa18ad82d0d9df66026141a41594',
}
BUILD = ROOT / 'build/setup'
LOG = BUILD / 'setup.log'


def step(message):
    print(message, flush=True)


def run(*command, cwd=None):
    LOG.parent.mkdir(parents=True, exist_ok=True)
    with LOG.open('a', encoding='utf-8') as stream:
        stream.write('\n$ ' + ' '.join(map(str, command)) + '\n')
        stream.flush()
        subprocess.run([str(c) for c in command], check=True, cwd=cwd, stdout=stream,
                       stderr=subprocess.STDOUT)


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def find_file(directory, name):
    matches = [p for p in directory.iterdir() if p.name.casefold() == name]
    return matches[0] if len(matches) == 1 else None


def fetch_sources():
    step('Preparing pinned sources and SDK patches...')
    return fetch_forza(ROOT, PINS, run)


def build_codegen_tool(sdk, jobs):
    if platform.system() == 'Windows':
        return windows_tools(ROOT, BUILD / 'windows-tools', download)[0]
    tool = BUILD / 'host/rexglue'
    if tool.is_file():
        return tool
    step('Building the code generator (a few minutes)...')
    flags = '-march=x86-64-v2'
    run('cmake', '-S', sdk, '-B', BUILD / 'host', '-G', 'Ninja',
        '-DCMAKE_C_COMPILER=clang', '-DCMAKE_CXX_COMPILER=clang++',
        f'-DCMAKE_C_FLAGS={flags}', f'-DCMAKE_CXX_FLAGS={flags}', '-DCMAKE_BUILD_TYPE=Release',
        '-DREXGLUE_ENABLE_TRACY=OFF', '-DREXGLUE_ENABLE_FIDELITYFX=OFF',
        '-DREXGLUE_ENABLE_PERF_COUNTERS=OFF', '-DREXGLUE_BUILD_TESTS=OFF')
    run('cmake', '--build', BUILD / 'host', '--target', 'rexglue', '-j', jobs)
    # The SDK writes its executables into its own out/ directory.
    found = sdk / 'out/linux-amd64/rexglue'
    if not found.is_file():
        raise ValueError('The code generator did not build; see the log')
    shutil.copy2(found, tool)
    return tool


def prepare_game(args):
    if args.game:
        game = args.game.resolve()
    else:
        game = BUILD / 'game'
        marker = BUILD / 'game.source'
        identity = f'{args.iso.resolve()}:{args.iso.stat().st_size}:{args.iso.stat().st_mtime_ns}'
        if not (game.is_dir() and marker.is_file() and marker.read_text() == identity):
            if game.exists():
                shutil.rmtree(game)
            step('Extracting your disc image...')
            run(sys.executable, ROOT / 'ps5/tools/extract-xiso.py', '--image', args.iso, '--output', game)
            marker.write_text(identity)
    for name, digest in GAME_FILES.items():
        path = find_file(game, name)
        if not path:
            raise ValueError(f'{name} is missing from the game files; is this Forza Horizon disc 1?')
        if sha(path) != digest:
            raise ValueError(f'{path.name} does not match the supported disc '
                             '(Forza Horizon, USA Xbox 360 disc 1, base version)')
    return game


def generate_code(source, tool, game):
    generated = BUILD / 'codegen/generated'
    receipt = BUILD / 'codegen/receipt.json'
    identity = {'compat': sha(ROOT / 'ps5/tools/codegen_compat.py'), 'tool': sha(tool), 'files': GAME_FILES, 'forza': PINS['forza']['revision']}
    if generated.is_dir() and receipt.is_file() and json.loads(receipt.read_text()) == identity:
        return generated
    step('Generating code from your game files...')
    work = BUILD / 'codegen'
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    recomp = source / 'forza-recomp'
    manifest = (recomp / 'forza_manifest.toml').read_text()
    for name in ('default.xex', 'XMediaFacade_default.xex', 'SpeechFacade_default.xex'):
        manifest = manifest.replace(f'../extracted/game/{name}', find_file(game, name.casefold()).as_posix())
    manifest = manifest.replace('game_root = "../extracted/game"', f'game_root = "{game.as_posix()}"')
    parsed = tomllib.loads(manifest)
    if [m['guest_path'] for m in parsed['modules']] != ['XMediaFacade_default.xex', 'SpeechFacade_default.xex']:
        raise ValueError('Unexpected upstream codegen manifest')
    shutil.copytree(recomp / 'hints', work / 'hints')
    (work / 'forza_manifest.toml').write_text(manifest)
    run(tool, 'codegen', work / 'forza_manifest.toml', cwd=work)
    if platform.system() == 'Windows':
        from codegen_compat import normalize
        step(f'Preserved Forza dot-product fix at {normalize(generated)} instructions.')
    receipt.write_text(json.dumps(identity))
    return generated


def download(pin, cache):
    archive = cache / Path(pin['url']).name
    if not archive.is_file() or sha(archive) != pin['sha256']:
        step(f'Downloading {archive.name}...')
        temporary = archive.with_suffix(archive.suffix + '.download')
        urllib.request.urlretrieve(pin['url'], temporary)
        if sha(temporary) != pin['sha256']:
            temporary.unlink()
            raise ValueError(f'Checksum mismatch for {archive.name}')
        temporary.replace(archive)
    return archive


def prepare_toolchain():
    cache = BUILD / 'tools'
    cache.mkdir(parents=True, exist_ok=True)
    compiler = cache / TOOLS['compiler']['directory'] / 'bin'
    if platform.system() == 'Windows':
        compiler = windows_tools(ROOT, BUILD / 'windows-tools', download)[1]
    elif not (compiler / 'clang++').is_file():
        with tarfile.open(download(TOOLS['compiler'], cache)) as archive:
            archive.extractall(cache, filter='data')
    sysroot = cache / TOOLS['sysroot']['directory']
    if not (sysroot / 'target/include/c++/v1').is_dir():
        unpack_zip(download(TOOLS['sysroot'], cache), cache)
    return compiler, sysroot


def package(game, module, output, app=None):
    step('Writing and checking the ready-to-copy folder...')
    finish_setup('PPSA99690', game, module, output, app)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source_group = parser.add_mutually_exclusive_group(required=True)
    source_group.add_argument('--iso', type=Path, help='Your Forza Horizon Xbox 360 disc 1 image (.iso)')
    source_group.add_argument('--game', type=Path, help='Already extracted disc files')
    parser.add_argument('--output', type=Path, default=ROOT.parent / 'Forza-PS5-prepared/PPSA99690',
                        help='New folder to write (ends in PPSA99690)')
    parser.add_argument('--jobs', type=int, default=max(1, min(8, os.cpu_count() or 1)))
    parser.add_argument('--app', type=Path, help='Optional matching app ZIP/folder for a complete title; omit if app is already installed')
    argv = None
    if len(sys.argv) == 1:
        argv = interactive_build('PPSA99690')
        if argv is None:
            print('Cancelled. No build started.')
            return
    args = parser.parse_args(argv)
    if platform.system() not in ('Linux', 'Windows') or platform.machine().lower() not in ('x86_64', 'amd64'):
        parser.error('ISO setup supports Linux x86-64 and native Windows x64')
    if sys.version_info < (3, 11):
        parser.error('Python 3.11 or newer is required')
    if sys.maxsize < 2**32:
        parser.error('Install 64-bit Python to run the native x64 tools')
    commands = ('git',) if platform.system() == 'Windows' else ('git', 'cmake', 'ninja', 'clang', 'clang++')
    for command in commands:
        if not shutil.which(command):
            parser.error(f'Missing required command: {command}')
    if platform.system() == 'Windows':
        check_windows_runtime()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    if args.iso and not args.iso.is_file():
        parser.error(f'Disc image not found: {args.iso}')
    if args.game and not args.game.is_dir():
        parser.error(f'Game folder not found: {args.game}')
    if args.output.name != 'PPSA99690':
        parser.error('--output must end in PPSA99690')
    if args.output.exists():
        parser.error(f'Output already exists: {args.output}; choose a new folder')
    if args.game and args.game.is_symlink():
        parser.error('Choose real extracted files, not a symbolic link')
    if args.game and (args.game.resolve() == args.output.resolve() or args.game.resolve() in args.output.resolve().parents):
        parser.error('Choose an output folder outside the game input')
    if args.app and args.app.is_dir() and (args.app.resolve() == args.output.resolve() or args.app.resolve() in args.output.resolve().parents):
        parser.error('Choose an output folder outside the app input')
    if args.app and (not args.app.exists() or args.app.is_symlink()):
        parser.error('--app must be a real matching app ZIP or folder')

    source, sdk = fetch_sources()
    tool = build_codegen_tool(sdk, args.jobs)
    game = prepare_game(args)
    generated = generate_code(source, tool, game)
    compiler, sysroot = prepare_toolchain()
    module = BUILD / 'module/forza.module'
    step('Compiling the game module (this is the long part)...')
    run(sys.executable, ROOT / 'ps5/tools/build-game-module.py', '--generated', generated,
        '--toolchain', compiler, '--sysroot', sysroot, '--sdk', sdk,
        '--work', BUILD / 'module', '--output', module, '--jobs', args.jobs)
    package(game, module, args.output, args.app)
    print(f'\nReady: {args.output.resolve()}\nNo console was contacted. See INSTALL.md for manual copying or optional FTP.')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f'Setup failed: {error}\nDetails: {LOG}', file=sys.stderr)
        sys.exit(1)
