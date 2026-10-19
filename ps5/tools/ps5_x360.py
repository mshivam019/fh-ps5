#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Checks and packaging for the two private Xbox 360 ports; no console access."""
import json
from pathlib import Path
import struct
import tempfile

PROFILES = json.loads(Path(__file__).with_name('x360-profiles.json').read_text(encoding='utf-8'))
LIMIT = 256 * 1024 * 1024


def check_module(path, profile):
    """Check the ELF container and the named ABI export used by the app loader."""
    size = path.stat().st_size
    if not 64 <= size <= LIMIT:
        raise ValueError('Invalid game module size')
    data = path.read_bytes()
    def region(offset, length):
        if offset < 0 or length < 0 or offset > len(data) or length > len(data) - offset:
            raise ValueError('Truncated game module')
        return data[offset:offset + length]
    header = struct.unpack('<16sHHIQQQIHHHHHH', region(0, 64))
    ident, kind, machine, version, _, phoff, shoff, _, ehsize, phsize, phcount, shsize, shcount, _ = header
    if (ident[:7] != b'\x7fELF\x02\x01\x01' or kind != 3 or machine != 62 or version != 1 or
            ehsize != 64 or phsize != 56 or shsize != 64 or not phcount or not shcount):
        raise ValueError('Expected an x86-64 ELF shared game module, not a disc file')
    region(phoff, phcount * phsize)
    region(shoff, shcount * shsize)
    loads = []
    for i in range(phcount):
        ptype, flags, offset, address, _, length, memory, _ = struct.unpack('<IIQQQQQQ', region(phoff + i * phsize, phsize))
        if ptype == 7:
            raise ValueError('Unsupported TLS in game module')
        if ptype != 1:
            continue
        region(offset, length)
        if length > memory or address + memory > LIMIT or flags & ~7:
            raise ValueError('Invalid game module load segment')
        if any(address < end and start < address + memory for start, end in loads):
            raise ValueError('Overlapping game module load segments')
        loads.append((address, address + memory))
    if not loads:
        raise ValueError('Game module has no loadable segments')
    sections = [struct.unpack('<IIQQQQIIQQ', region(shoff + i * shsize, shsize)) for i in range(shcount)]
    tables = [s for s in sections if s[1] == 11]
    if len(tables) != 1:
        raise ValueError('Expected one dynamic symbol table in the game module')
    symbols = tables[0]
    if symbols[9] != 24 or symbols[5] % 24 or symbols[6] >= len(sections):
        raise ValueError('Invalid game module symbol table')
    strings_section = sections[symbols[6]]
    if strings_section[1] != 3:
        raise ValueError('Invalid game module string table')
    strings = region(strings_section[4], strings_section[5])
    region(symbols[4], symbols[5])
    defined = {}
    for offset in range(symbols[4], symbols[4] + symbols[5], 24):
        name_offset, _, _, section, address, length = struct.unpack('<IBBHQQ', region(offset, 24))
        end = strings.find(b'\0', name_offset)
        if name_offset >= len(strings) or end < 0:
            raise ValueError('Invalid game module symbol name')
        if section == 0:
            continue
        name = strings[name_offset:end].decode('ascii', errors='strict')
        if name not in profile['exports']:
            continue
        if (section >= len(sections) or not length or
                not any(start <= address and address + length <= stop for start, stop in loads)):
            raise ValueError('Invalid required game module export: ' + name)
        if name in defined:
            raise ValueError('Duplicate required game module export: ' + name)
        defined[name] = (section, address, length)
    missing = set(profile['exports']) - set(defined)
    if missing:
        raise ValueError('Wrong/incomplete game module; missing exports: ' + ', '.join(sorted(missing)))
    section, address, length = defined[profile['abi_symbol']]
    target = sections[section]
    relative = address - target[3]
    expected = profile['abi'].encode('ascii') + b'\0'
    if (target[1] == 8 or relative < 0 or relative + len(expected) > target[5] or
            length < len(expected) or region(target[4] + relative, len(expected)) != expected):
        raise ValueError('Game module does not match the released app. Use setup for ' + profile['app_release'])
    return profile['abi']


def check_app(title, root):
    from ps5_install_lib import digest
    if digest(root / 'eboot.bin') != PROFILES[title]['app_sha256']:
        raise ValueError('App executable does not match release ' + PROFILES[title]['app_release'])


def prepared_game_files(title, source, module=None):
    from ps5_install_lib import child, digest, files, personal_path
    profile = PROFILES[title]
    if source.is_symlink():
        raise ValueError('Choose real game files, not a symbolic link')
    if not source.is_dir():
        raise ValueError('Choose a prepared game folder. An ISO must go through setup.sh first.')
    if not any(p.name.casefold() == 'default.xex' for p in source.iterdir()):
        source = source / 'game'
    selected = files(source)
    for name, expected in profile['guest_sha256'].items():
        file = child(source, name)
        if not file.is_file() or digest(file) != expected:
            raise ValueError('Unsupported or damaged ' + name + ' for ' + profile['name'])
    module = module or source / profile['module']
    if module.is_symlink():
        raise ValueError('Game module must not be a symbolic link')
    check_module(module, profile)
    items = []
    for file in selected:
        rel = file.relative_to(source)
        if (rel.parts[0].casefold() == '$systemupdate' or personal_path(rel, title) or
                '.before-' in file.name or file.name.casefold() == profile['module'].casefold()):
            continue
        items.append((file, 'game/' + rel.as_posix()))
    items.append((module, 'game/' + profile['module']))
    return items


def finish_setup(title, game, module, output, app=None):
    from ps5_install_lib import unpack_app, validate_names, write_prepared_folder, validate_prepared
    if output.name != title:
        raise ValueError('Output must end in ' + title)
    for source in (game, app):
        if source and source.is_dir() and (output.resolve() == source.resolve() or source.resolve() in output.resolve().parents):
            raise ValueError('Choose an output outside the input folder')
    report = output.parent / ('VALIDATION-' + title + '.json')
    if report.exists() or report.is_symlink():
        raise ValueError('Validation report already exists; choose a new output folder')
    with tempfile.TemporaryDirectory(prefix='ps5-x360-app-') as temporary:
        items = unpack_app(app, title, Path(temporary)) if app else []
        items += prepared_game_files(title, game, module)
        selected = {name: path for path, name in items}
        items = [(path, name) for name, path in sorted(selected.items())]
        validate_names(items)
        write_prepared_folder(items, output.parent, title, 'full' if app else 'game')
    result = validate_prepared(output, output.parent / ('MANIFEST-' + title + '.json'))
    report = output.parent / ('VALIDATION-' + title + '.json')
    with report.open('x', encoding='utf-8') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')
    if not result['ok']:
        raise ValueError('Prepared-folder checks failed: ' + '; '.join(result['errors']))
    print('READY: copy and layout checks passed; module ABI matches app ' + PROFILES[title]['app_release'])
    print('Copy the prepared folder with any file manager, USB or FTP method. Keep your saves/settings.')
    print('Your generated module and disc contents are private; do not share them.')


def interactive_build(title):
    from ps5_store_setup import choose_path
    profile = PROFILES[title]
    def ask(prompt, folder):
        try:
            return choose_path(prompt, folder)
        except OSError:
            value = input(prompt + ' (blank to cancel): ').strip().strip('"').strip("'")
            return Path(value) if value else None
    iso = ask(profile['name'] + ': choose your Xbox 360 ISO', False)
    if iso is None:
        return None
    parent = ask('Choose an output location on this computer or USB drive', True)
    if parent is None:
        return None
    from ps5_store_setup import next_output
    return ['--iso', str(iso), '--output', str(next_output(parent, title) / title)]
