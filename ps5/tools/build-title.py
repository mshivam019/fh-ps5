#!/usr/bin/env python3
"""Build and sign the native Forza app (runtime only; game code is a separate private module)."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--jobs', type=int, default=4)
parser.add_argument('--link-only', action='store_true')
args = parser.parse_args()
if args.jobs < 1:
    parser.error('jobs must be positive')
env = os.environ.copy()
sdk = Path(env.setdefault('PS5_PAYLOAD_SDK', str(ROOT.parent / 'Links-Awakening-PS5/.deps/native/ps5-payload-sdk')))
env['FORZA_BUILD_JOBS'] = str(args.jobs)
def run(*command):
    subprocess.run(list(map(str, command)), check=True, env=env)
if not args.link_only:
    run('bash', ROOT / 'ps5/tools/build-runtime.sh')
    run(sys.executable, ROOT / 'ps5/tools/build-game-objects.py', '--jobs', args.jobs)
build = ROOT / 'build/ps5-game'
rows = json.loads((build / 'compile_commands.json').read_text())
archives = []
for name in ('forza_app',):
    objects = [Path(row['output']) for row in rows
               if f'/app/CMakeFiles/{name}.dir/' in row['output'] and row['output'].endswith('.o')]
    if not objects or any(not obj.is_file() for obj in objects):
        raise SystemExit(f'Missing native objects for {name}')
    archive = build / f'lib{name}.a'
    # Build a fresh archive so removed source objects cannot survive a relink.
    temporary = archive.with_suffix('.a.new')
    if temporary.exists():
        temporary.unlink()
    run('llvm-ar', 'rcs', temporary, *objects)
    temporary.replace(archive)
    archives.append(archive)
runtime = ROOT / '.deps/rexglue-sdk/out/ps5-amd64'
required = [runtime / 'librexruntime.a', runtime / 'librexgpu-xenos.a']
if any(not p.is_file() for p in required):
    raise SystemExit('Build the native runtime and Xenos Vulkan archives first')
dependencies = sorted(p for p in runtime.glob('*.a') if p not in required)
ffmpeg = ROOT / 'build/ps5-ffmpeg/install/lib'
dependencies += [ffmpeg / f'lib{name}.a' for name in ('avcodec', 'avutil')]
dependencies += [ROOT.parent / 'Dusklight-PS5/build/sdl3-ps5/libSDL3.a']
if any(not p.is_file() for p in dependencies):
    raise SystemExit('Missing native dependency archive')
run('bash', ROOT / 'ps5/tools/link-title.sh', ROOT / 'build/native-title',
    ROOT.parent / 'PS5_Vulkan', sdk, '--whole-archive', *archives, *required,
    '--no-whole-archive', '--start-group', *dependencies, '--end-group')
print('App executable:', ROOT / 'build/native-title/eboot.bin')
