#!/usr/bin/env python3
"""Compile the native app objects."""
import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--jobs', type=int, default=4)
args = parser.parse_args()
if args.jobs < 1:
    parser.error('jobs must be positive')
build = ROOT / 'build/ps5-game'
env = os.environ.copy()
env.setdefault('PS5_PAYLOAD_SDK', str(ROOT.parent / 'Links-Awakening-PS5/.deps/native/ps5-payload-sdk'))
subprocess.run(['cmake', '-S', str(ROOT / 'ps5/runtime'), '-B', str(build), '-G', 'Ninja',
                f'-DCMAKE_TOOLCHAIN_FILE={ROOT}/ps5/cmake/native.cmake',
                '-DCMAKE_BUILD_TYPE=Release', '-DFORZA_BUILD_APP=ON',
                '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'], check=True, env=env)
rows = json.loads((build / 'compile_commands.json').read_text())
targets = [str(Path(row['output']).relative_to(build)) for row in rows
           if '/app/CMakeFiles/forza_app.dir/' in row['output']]
if not targets:
    raise SystemExit('No native app compile targets found')
print(f'Compiling {len(targets)} app objects', flush=True)
result = subprocess.run(['ninja', '-C', str(build), '-j', str(args.jobs), *targets], env=env)
raise SystemExit(result.returncode)
