# SPDX-License-Identifier: MIT
import copy
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ps5_x360 as x360
import ps5_install_lib as helper
import ps5_store_setup as store


class XboxSetupTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.profiles = copy.deepcopy(x360.PROFILES)
        self.patcher = patch.dict(x360.PROFILES, self.profiles)
        self.patcher.start()
        self.addCleanup(self.patcher.stop)

    def game(self, title):
        p = self.root / title / 'game'
        p.mkdir(parents=True)
        profile = x360.PROFILES[title]
        for name in profile['guest_sha256']:
            data = ('synthetic ' + title + name).encode()
            (p / name).write_bytes(data)
            profile['guest_sha256'][name] = hashlib.sha256(data).hexdigest()
        c = self.root / (title + '.c')
        c.write_text('const char ' + profile['abi_symbol'] + '[] = "' + profile['abi'] + '";\n' +
                     '\n'.join('int ' + name + ' = 1;' for name in profile['exports'] if name != profile['abi_symbol']))
        subprocess.run(['clang', '-shared', '-fPIC', '-nostdlib', str(c), '-o', str(p / profile['module'])], check=True)
        (p / 'assets').mkdir()
        (p / 'assets/data').write_bytes(b'game fixture content')
        for name in ('user/save', '$SystemUpdate/update', 'logs/log'):
            target = p / name
            target.parent.mkdir(exist_ok=True)
            target.write_bytes(b'keep private')
        return p

    def app(self, title):
        self.profiles[title]['app_sha256'] = hashlib.sha256(b'app').hexdigest()
        path = self.root / (title + '.zip')
        with zipfile.ZipFile(path, 'w') as z:
            for name, data in [('eboot.bin', 'app'), ('sce_sys/icon0.png', 'icon'),
                               ('sce_sys/param.json', json.dumps({'titleId': title}))]:
                z.writestr(title + '/' + name, data)
        return path

    def test_package_both_titles_game_and_full_and_recheck_corruption(self):
        for title in self.profiles:
            game = self.game(title)
            for full in (False, True):
                out = self.root / (title + str(full)) / title
                with patch('ftplib.FTP', side_effect=AssertionError('network access')):
                    x360.finish_setup(title, game, game / self.profiles[title]['module'], out,
                                      self.app(title) if full else None)
                manifest = out.parent / ('MANIFEST-' + title + '.json')
                result = helper.validate_prepared(out, manifest)
                self.assertTrue(result['ok'], result)
                self.assertEqual(result['mode'], 'full' if full else 'game')
                self.assertFalse((out / 'game/user').exists())
                self.assertFalse((out / 'game/$SystemUpdate').exists())
                self.assertFalse((out / 'game/logs').exists())
                (out / 'game/assets/data').write_bytes(b'corruption')
                self.assertFalse(helper.validate_prepared(out, manifest)['ok'])

    def test_wrong_disc_module_and_truncation_rejected(self):
        title = 'PPSA99680'
        game = self.game(title)
        profile = self.profiles[title]
        module = game / profile['module']
        original = module.read_bytes()
        module.write_bytes(original.replace(profile['abi'].encode(), b'0' * 64))
        with self.assertRaisesRegex(ValueError, 'does not match'):
            x360.prepared_game_files(title, game)
        module.write_bytes(original[:100])
        with self.assertRaises(ValueError):
            x360.check_module(module, profile)
        module.write_bytes(original)
        (game / 'default.xex').write_bytes(b'wrong disc')
        with self.assertRaisesRegex(ValueError, 'default.xex'):
            x360.prepared_game_files(title, game)

    def test_other_game_module_rejected(self):
        first = self.game('PPSA99680')
        self.game('PPSA99690')
        with self.assertRaisesRegex(ValueError, 'missing exports'):
            x360.check_module(first / 'naruto.module', self.profiles['PPSA99690'])

    def test_nested_output_and_symlinks_preserve_source(self):
        title = 'PPSA99690'
        game = self.game(title)
        with self.assertRaisesRegex(ValueError, 'outside'):
            x360.finish_setup(title, game, game / 'forza.module', game / title)
        self.assertFalse((game / title).exists())
        link = self.root / 'linked'
        link.symlink_to(game, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'symbolic link'):
            x360.prepared_game_files(title, link)
        (game / 'assets/link').symlink_to(game / 'default.xex')
        with self.assertRaises(ValueError):
            x360.prepared_game_files(title, game)

    def test_reuse_parent_folder_and_numbered_output(self):
        title = 'PPSA99680'
        game = self.game(title)
        first = store.prepare_store_game(title, game.parent, self.root)
        before = (first.parent / 'MANIFEST-PPSA99680.json').read_bytes()
        second = store.prepare_store_game(title, game, self.root)
        self.assertNotEqual(first, second)
        self.assertNotIn(':', first.parent.name)
        self.assertEqual(before, (first.parent / 'MANIFEST-PPSA99680.json').read_bytes())

    def test_cancel_does_not_build(self):
        for choices in ([None], [self.root / 'disc.iso', None]):
            with patch.object(store, 'choose_path', side_effect=choices):
                self.assertIsNone(x360.interactive_build('PPSA99680'))
        self.assertEqual(list(self.root.iterdir()), [])

    def test_app_input_cannot_contain_output(self):
        title = 'PPSA99690'
        game = self.game(title)
        app = self.root / 'app'
        app.mkdir()
        with self.assertRaisesRegex(ValueError, 'outside'):
            x360.finish_setup(title, game, game / 'forza.module', app / title, app)
        self.assertFalse((app / title).exists())

    def test_overlapping_load_segments_rejected(self):
        game = self.game('PPSA99690')
        module = game / 'forza.module'
        data = bytearray(module.read_bytes())
        phoff = struct.unpack_from('<Q', data, 32)[0]
        count = struct.unpack_from('<H', data, 56)[0]
        loads = [phoff + i * 56 for i in range(count) if struct.unpack_from('<I', data, phoff + i * 56)[0] == 1]
        self.assertGreater(len(loads), 1)
        struct.pack_into('<Q', data, loads[1] + 16, struct.unpack_from('<Q', data, loads[0] + 16)[0])
        module.write_bytes(data)
        with self.assertRaisesRegex(ValueError, 'Overlapping'):
            x360.check_module(module, self.profiles['PPSA99690'])

    def test_wrong_app_release_rejected(self):
        title = 'PPSA99680'
        game = self.game(title)
        app = self.app(title)
        self.profiles[title]['app_sha256'] = '0' * 64
        out = self.root / 'wrong-app' / title
        with self.assertRaisesRegex(ValueError, 'App executable'):
            x360.finish_setup(title, game, game / 'naruto.module', out, app)
        self.assertFalse(out.exists())
