# SPDX-License-Identifier: MIT
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import native_setup as native


class NativeSetupTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_response_file_quotes_spaces_slashes_and_rejects_control_characters(self):
        path = self.root / 'link args.rsp'
        arg = native.response_file(path, ['-shared', 'C:\\My Files\\game.o', 'a"b'])
        self.assertEqual(arg, '@' + str(path))
        self.assertEqual(path.read_text(), '"-shared"\n"C:\\\\My Files\\\\game.o"\n"a\\"b"\n')
        for bad in ['a\nb', 'a\rb', 'a\0b']:
            with self.assertRaises(ValueError):
                native.response_file(path, [bad])

    def test_unsafe_tool_archives_rejected_before_extracting(self):
        for bad in ['../escape', '/absolute', 'C:/drive', 'dir\\..\\escape']:
            archive = self.root / 'bad.zip'
            with zipfile.ZipFile(archive, 'w') as z:
                z.writestr('safe.txt', 'safe')
                z.writestr(bad, 'bad')
            dest = self.root / 'unpacked'
            with self.assertRaises(ValueError):
                native.unpack_zip(archive, dest)
            self.assertFalse(dest.exists())

    def test_tool_archive_symlink_rejected(self):
        archive = self.root / 'link.zip'
        with zipfile.ZipFile(archive, 'w') as z:
            info = zipfile.ZipInfo('link')
            info.external_attr = 0o120777 << 16
            z.writestr(info, '../outside')
        with self.assertRaises(ValueError):
            native.unpack_zip(archive, self.root / 'out')

    def test_windows_tools_use_pinned_downloads_and_reuse_extraction(self):
        config = self.root / 'ps5/tools'
        config.mkdir(parents=True)
        pins = {}
        archives = {}
        for key, filename in [('codegen', 'rexglue.exe'), ('compiler', 'clang++.exe')]:
            pin = {'url': key, 'sha256': key + '-digest', 'directory': key + '-root'}
            pins[key] = pin
            archive = self.root / (key + '.zip')
            with zipfile.ZipFile(archive, 'w') as z:
                z.writestr(pin['directory'] + '/bin/' + filename, key)
            archives[key] = archive
        (config / 'windows-tools.json').write_text(json.dumps(pins))
        calls = []
        def download(pin, cache):
            calls.append(pin)
            return archives[pin['url']]
        first = native.windows_tools(self.root, self.root / 'cache', download)
        self.assertTrue(first[0].is_file())
        self.assertTrue((first[1] / 'clang++.exe').is_file())
        stamp = first[0].stat().st_mtime_ns
        self.assertEqual(native.windows_tools(self.root, self.root / 'cache', download), first)
        self.assertEqual(first[0].stat().st_mtime_ns, stamp)
        self.assertEqual(calls, [pins['codegen'], pins['compiler']] * 2)

    def test_source_preparation_applies_patches_without_shell_and_is_repeatable(self):
        source = self.root / 'upstream/forza'
        sdk = self.root / '.deps/rexglue-sdk'
        patches = source / 'patches/rexglue-sdk'
        patches.mkdir(parents=True)
        sdk.mkdir(parents=True)
        (sdk / 'header.h').write_text('old\n')
        patch = 'diff --git a/header.h b/header.h\n--- a/header.h\n+++ b/header.h\n@@ -1 +1 @@\n-old\n+upstream\n'
        (patches / '0001.patch').write_text(patch)
        ps5 = self.root / 'ps5/patches'
        ps5.mkdir(parents=True)
        (ps5 / 'rexglue-ps5.patch').write_text(patch.replace('-old\n+upstream', '-upstream\n+ps5'))
        pins = {}
        def run(*args):
            self.assertEqual(args[0], 'git')
            subprocess.run(list(map(str, args)), check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for directory, key in [(source, 'forza'), (sdk, 'rexglue_sdk')]:
            run('git', 'init', directory)
            run('git', '-C', directory, 'add', '.')
            run('git', '-C', directory, '-c', 'user.name=Test', '-c', 'user.email=test@example.invalid', 'commit', '-m', 'fixture')
            revision = subprocess.check_output(['git', '-C', str(directory), 'rev-parse', 'HEAD'], text=True).strip()
            pins[key] = {'revision': revision, 'url': 'unused'}
        self.assertEqual(native.fetch_forza(self.root, pins, run), (source, sdk))
        self.assertEqual((sdk / 'header.h').read_text(), 'ps5\n')
        self.assertEqual(native.fetch_forza(self.root, pins, run), (source, sdk))
        (sdk / 'header.h').write_text('user edit\n')
        with self.assertRaisesRegex(ValueError, 'unfinished edits'):
            native.fetch_forza(self.root, pins, run)
        self.assertEqual((sdk / 'header.h').read_text(), 'user edit\n')

    def test_real_linker_reads_response_file_with_spaces(self):
        import shutil
        if not shutil.which('clang') or not shutil.which('ld.lld'):
            self.skipTest('Host compiler/linker required')
        source = self.root / 'with spaces.c'
        source.write_text('int answer(void) { return 42; }')
        obj = self.root / 'with spaces.o'
        subprocess.run(['clang', '-fPIC', '-c', str(source), '-o', str(obj)], check=True)
        output = self.root / 'with spaces.so'
        response = native.response_file(self.root / 'link.rsp', ['-shared', obj, '-o', output])
        subprocess.run(['ld.lld', '--rsp-quoting=posix', response], check=True)
        self.assertEqual(output.read_bytes()[:4], b'\x7fELF')

    def test_disc_names_reject_windows_device_and_drive_paths(self):
        import importlib.util
        import io
        import struct
        path = Path(__file__).resolve().parents[1] / 'extract-xiso.py'
        spec = importlib.util.spec_from_file_location('extract_xiso', path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        for name in ['C:escape', 'NUL', 'con.txt', 'trailing.', 'file:stream', 'x\\y']:
            data = struct.pack('<HHIIBB', 0, 0, 0, 1, 0, len(name)) + name.encode()
            with self.assertRaises(ValueError):
                list(module.read_directory(io.BytesIO(data), 0, 0, len(data)))
        name = 'default.xex'
        data = struct.pack('<HHIIBB', 0, 0, 0, 1, 0, len(name)) + name.encode()
        self.assertEqual(list(module.read_directory(io.BytesIO(data), 0, 0, len(data)))[0][0], name)

    def test_missing_windows_runtime_has_actionable_error(self):
        import ctypes
        from unittest.mock import patch
        with patch.object(native.os, 'name', 'nt'), patch.object(ctypes, 'WinDLL', create=True) as dll:
            dll.side_effect = OSError('missing DLL')
            with self.assertRaisesRegex(ValueError, 'https://aka.ms/vc14/vc_redist.x64.exe'):
                native.check_windows_runtime()
            self.assertEqual(dll.call_count, 4)
            dll.side_effect = None
            native.check_windows_runtime()
