#!/usr/bin/env python3
"""Namespace guest helper functions that collide when the game executable and
its two facade DLLs are linked into one static module."""
import argparse
import json
from pathlib import Path
import re

# Module directory -> prefix for its names that another module also defines.
MODULES = {'default': None, 'xmediafacade': 'forza_xmf_', 'speechfacade': 'forza_sf_'}

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--generated', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()


def functions(module):
    result = set()
    for source in (args.generated / module).glob('forza_recomp.*.cpp'):
        result.update(re.findall(r'DEFINE_REX_FUNC\((\w+)\)', source.read_text()))
    return result


defined = {module: functions(module) for module in MODULES}
maps = {}
for module, prefix in MODULES.items():
    if not prefix:
        continue
    others = set().union(*(names for other, names in defined.items() if other != module))
    shared = defined[module] & others
    if not shared:
        raise SystemExit(f'Expected shared guest helper names in {module}; inspect codegen changes')
    names = {}
    for name in shared:
        names[name] = prefix + name
        names['__imp__' + name] = '__imp__' + prefix + name
    maps[module] = names

args.output.mkdir(parents=True, exist_ok=True)
for module in MODULES:
    names = maps.get(module, {})
    pattern = (re.compile(r'\b(?:' + '|'.join(re.escape(n) for n in sorted(names, key=len, reverse=True)) + r')\b')
               if names else None)
    destination = args.output / module
    destination.mkdir(exist_ok=True)
    for path in (args.generated / module).iterdir():
        if path.suffix not in {'.h', '.cpp'}:
            continue
        text = path.read_text()
        if pattern:
            text = pattern.sub(lambda match: names[match[0]], text)
        text = text.replace(
            '#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)',
            '#if REX_PLATFORM_PS5 || REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)')
        target = destination / path.name
        if not target.exists() or target.read_text() != text:
            target.write_text(text)
(args.output / 'static-name-map.json').write_text(json.dumps(maps, indent=2, sort_keys=True) + '\n')
print('Namespaced shared guest functions: ' +
      ', '.join(f'{module} {len(names) // 2}' for module, names in maps.items()))
