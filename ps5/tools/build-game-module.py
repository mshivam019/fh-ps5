#!/usr/bin/env python3
"""Compile code generated from your own disc into the private game module
(forza.module) that the PS5 app loads. Never publish its output."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import importlib.util
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
UPSTREAM = ROOT / 'upstream/forza/forza-recomp'
# Generated module directory -> symbol prefix of its entry points (None: the executable).
MODULES = {'default': None, 'xmediafacade': 'ForzaXMF', 'speechfacade': 'ForzaSF'}
EXPORTS = ['forza_module_abi', 'PPCImageConfig'] + [
    f'{prefix}_{name}' for prefix in MODULES.values() if prefix
    for name in ('Register', 'GetImageInfo')]
# Upstream sources with guest hooks and kernel overrides that call generated code. The
# desktop-only parts (window, Vulkan hooks, overlays, fault guard, tracing) stay out;
# ps5/module/forza_ps5.cpp replaces what they provide to these files.
HOOK_SOURCES = [*sorted((UPSTREAM / 'src/hooks').glob('*.cpp')),
                *sorted((UPSTREAM / 'src/kernel').glob('*.cpp')),
                *(UPSTREAM / 'src/debug' / name for name in (
                    'codriver_log.cpp', 'dlc_log.cpp', 'frame_stats.cpp', 'audio_level.cpp')),
                UPSTREAM / 'src/platform/socket.cpp',
                ROOT / 'ps5/module/forza_ps5.cpp',
                ROOT / 'ps5/module/title_waits.cpp']
HOOK_FLAGS = ['-DFORZA_TITLE_UPDATE=0', f'-I{UPSTREAM / "src"}']


def module_flags(module, directory):
    prefix = MODULES[module]
    flags = [f'-I{directory}']
    if prefix:
        flags += [f'-DReXModule_Register={prefix}_Register',
                  f'-DReXModule_GetImageInfo={prefix}_GetImageInfo',
                  f'-DPPCImageConfig={prefix}_ImageConfig',
                  f'-DPPCFuncMappings={prefix}_FuncMappings']
    return flags


def load_support():
    spec = importlib.util.spec_from_file_location('module_support', ROOT / 'ps5/tools/module-support.py')
    support = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(support)
    return support


def sha(data):
    return hashlib.sha256(data).hexdigest()


def run(command, log):
    with log.open('a', encoding='utf-8') as stream:
        subprocess.run([str(c) for c in command], check=True, stdout=stream, stderr=subprocess.STDOUT)


def symbol_names(nm, path, *flags):
    text = subprocess.check_output([str(nm), *flags, str(path)], text=True)
    return {line.split()[-1] for line in text.splitlines() if line.strip()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--generated', required=True, type=Path, help='rexglue codegen output (default/, xmediafacade/, speechfacade/)')
    parser.add_argument('--toolchain', required=True, type=Path, help='LLVM bin directory (clang++, ld.lld, llvm-nm)')
    parser.add_argument('--sysroot', required=True, type=Path, help='PS5 payload SDK')
    parser.add_argument('--sdk', required=True, type=Path, help='Patched ReXGlue SDK source')
    parser.add_argument('--work', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--write-imports', action='store_true',
                        help='Developer mode: record the module imports as the app contract')
    args = parser.parse_args()
    support = load_support()
    tools = {name: args.toolchain / name for name in ('clang++', 'ld.lld', 'llvm-nm')}
    for name, path in tools.items():
        if not path.is_file():
            parser.error(f'Missing {name} in {args.toolchain}')
    args.work.mkdir(parents=True, exist_ok=True)
    log = args.work / 'build.log'

    replacements = {'{SYSROOT}': str(args.sysroot.resolve()), '{SDK}': str(args.sdk.resolve())}
    flags = []
    for line in support.FLAGS.read_text().splitlines():
        if not line or line.startswith('#'):
            continue
        for key, value in replacements.items():
            line = line.replace(key, value)
        flags.append(line)

    generated = args.work / 'generated'
    run([sys.executable, ROOT / 'ps5/tools/prepare-static-module.py', '--generated', args.generated,
         '--output', generated], log)

    objects_dir = args.work / 'objects'
    objects_dir.mkdir(exist_ok=True)
    clang = tools['clang++']
    identity_base = sha('\n'.join(flags).encode() + clang.read_bytes()[:4096] +
                        (args.sdk / 'include/rex/ppc.h').read_bytes())

    jobs = []  # (source, object, extra flags, pch)
    for module in MODULES:
        directory = generated / module
        extra = module_flags(module, directory)
        header = directory / 'forza_pch.h'
        pch = objects_dir / f'{module}.pch'
        pch_identity = sha((identity_base + str(extra)).encode() + header.read_bytes())
        stamp = pch.with_suffix('.sha256')
        if not (pch.is_file() and stamp.is_file() and stamp.read_text() == pch_identity):
            print(f'Preparing {module} headers...', flush=True)
            run([clang, *flags, *extra, '-x', 'c++-header', header, '-o', pch], log)
            stamp.write_text(pch_identity)
        for source in sorted(directory.glob('*.cpp')):
            jobs.append((source, objects_dir / f'{module}-{source.stem}.o', extra, pch))
    hook_flags = [*HOOK_FLAGS, f'-I{generated / "default"}']
    for source in HOOK_SOURCES:
        jobs.append((source, objects_dir / f'hook-{source.parent.name}-{source.stem}.o', hook_flags, None))
    abi_source = args.work / 'module_abi.cpp'

    def compile_one(job):
        source, obj, extra, pch = job
        identity = sha((identity_base + str(extra)).encode() + source.read_bytes() +
                       (pch.with_suffix('.sha256').read_bytes() if pch else b''))
        stamp = obj.with_suffix('.sha256')
        if obj.is_file() and stamp.is_file() and stamp.read_text() == identity:
            return obj
        command = [clang, *flags, *extra]
        if pch:
            command += ['-include-pch', pch]
        run([*command, '-c', source, '-o', obj], log)
        stamp.write_text(identity)
        return obj

    print(f'Compiling {len(jobs)} game code files with {args.jobs} jobs (this takes a while)...', flush=True)
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        objects = list(executor.map(compile_one, jobs))

    version_script = args.work / 'exports.map'
    version_script.write_text('{ global: ' + ' '.join(n + ';' for n in EXPORTS) + ' local: *; };\n')

    def link():
        abi_source.write_text('extern "C" __attribute__((visibility("default"))) const char '
                              f'forza_module_abi[] = "{support.abi()}";\n')
        abi_object = args.work / 'module_abi.o'
        run([clang, *flags, '-c', abi_source, '-o', abi_object], log)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        run([tools['ld.lld'], '-shared', '-z', 'max-page-size=16384', '-z', 'common-page-size=16384',
             '--hash-style=sysv', f'--version-script={version_script}', *objects, abi_object,
             '-o', args.output], log)

    if args.write_imports:
        # Record the SDK headers the module compiles against; the ABI identity
        # hashes their contents.
        sdk_root = args.sdk.resolve()
        headers = set()
        for source, _, extra, _ in [(generated / m / 'forza_pch.h', None,
                                     module_flags(m, generated / m), None) for m in MODULES] + [
                                        job for job in jobs if job[3] is None]:
            deps = subprocess.check_output([str(clang), *flags, *extra, '-x', 'c++', '-M', str(source)],
                                           text=True)
            for token in deps.replace('\\\n', ' ').split():
                path = Path(token).resolve() if not token.endswith(':') else None
                if path and path.is_file() and path.is_relative_to(sdk_root):
                    headers.add(path.relative_to(sdk_root).as_posix())
        # Kernel imports the hooks override and may forward to the runtime's own
        # implementation (forza::SdkImport): every one the runtime also defines.
        hook_objects = [obj for (source, _, _, pch), obj in zip(jobs, objects) if pch is None]
        overridden = set()
        for obj in hook_objects:
            overridden |= {n for n in symbol_names(tools['llvm-nm'], obj, '--defined-only')
                           if n.startswith('__imp__') and not n.startswith('__imp__sub_')}
        runtime_archive = args.sdk / 'out/ps5-amd64/librexruntime.a'
        provided = symbol_names(tools['llvm-nm'], runtime_archive, '--defined-only')
        support.LOOKUPS.write_text(
            '# Runtime implementations of the kernel imports the game module overrides\n'
            '# (forza::SdkImport). Generated by ps5/tools/build-game-module.py --write-imports.\n' +
            ''.join(name + '\n' for name in sorted(overridden & provided)))
        support.HEADERS.write_text('# ReXGlue SDK headers the game module is compiled against.\n'
                                   '# Generated by ps5/tools/build-game-module.py --write-imports.\n' +
                                   ''.join(name + '\n' for name in sorted(headers)))
    link()
    undefined = symbol_names(tools['llvm-nm'], args.output, '-D', '--undefined-only')
    if args.write_imports:
        support.IMPORTS.write_text('# Runtime symbols the game module imports from the app.\n'
                                   '# Generated by ps5/tools/build-game-module.py --write-imports.\n' +
                                   ''.join(name + '\n' for name in sorted(undefined)))
        link()  # the ABI identity covers the import list
    else:
        missing = undefined - set(support.imports())
        if missing:
            raise ValueError('Game module needs runtime symbols this app does not provide: ' +
                             ', '.join(sorted(missing)[:20]))
    exported = symbol_names(tools['llvm-nm'], args.output, '-D', '--defined-only')
    if set(EXPORTS) - exported:
        raise ValueError('Missing game module exports: ' + ', '.join(sorted(set(EXPORTS) - exported)))
    print(f'Game module: {args.output} ({args.output.stat().st_size} bytes, {len(undefined)} runtime imports)')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f'Module build failed: {error}', file=sys.stderr)
        sys.exit(1)
