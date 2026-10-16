#!/usr/bin/env python3
"""Developer deploy: copy the app, the private game module and the game files to
the console as PPSA99690 and register it. Uploads only files whose size differs;
eboot.bin and forza.module are always uploaded and read back to check their hash. Never touches user/ (saves)."""
import argparse
import ftplib
import hashlib
import json
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
TITLE = 'PPSA99690'
REMOTE = f'/mnt/ext1/etaHEN/games/{TITLE}'
HOST = os.environ.setdefault('PS5_HOST', '192.168.50.6')
sys.path.insert(0, str(ROOT.parent / 'Wind-Waker-HD-PS5/ps5/tools'))
from console_control import call  # noqa: E402


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def remote_sha256(path):
    digest = hashlib.sha256()
    offset = 0
    while True:
        chunk = call(48, {'path': path, 'offset': offset, 'limit': 1 << 20})
        if not chunk:
            return digest.hexdigest()
        digest.update(chunk)
        offset += len(chunk)


def remote_sizes(ftp, path):
    """File sizes in a remote directory (FTP LIST; the control API caps listings at 256)."""
    lines = []
    try:
        ftp.retrlines(f'LIST {path}', lines.append)
    except ftplib.error_perm:
        return {}
    sizes = {}
    for line in lines:
        fields = line.split(None, 8)
        if len(fields) == 9 and line.startswith('-'):
            sizes[fields[8]] = int(fields[4])
    return sizes


def ensure_dir(ftp, path, known):
    if path in known:
        return
    parent = path.rsplit('/', 1)[0]
    if parent and parent not in known:
        ensure_dir(ftp, parent, known)
    try:
        ftp.mkd(path)
    except ftplib.error_perm:
        pass  # exists
    known.add(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--skip-game', action='store_true', help='Do not compare or upload game/ files')
    args = parser.parse_args()
    files = {
        'eboot.bin': ROOT / 'build/native-title/eboot.bin',
        'sce_module/libc.prx': ROOT.parent / 'PS5_Vulkan/runtime/libc.prx',
        'sce_sys/param.json': ROOT / 'ps5/sce_sys/param.json',
        'sce_sys/icon0.png': ROOT / 'ps5/sce_sys/icon0.png',
        'sce_sys/pic0.png': ROOT / 'ps5/sce_sys/pic0.png',
        'sce_sys/pic1.png': ROOT / 'ps5/sce_sys/pic1.png',
        'forza.toml': ROOT / 'ps5/config/forza.toml',
        'game/forza.module': ROOT / 'build/ps5-module/forza.module',
    }
    if not args.skip_game:
        game = ROOT / 'private/game'
        for path in sorted(game.rglob('*')):
            if path.is_file():
                files['game/' + path.relative_to(game).as_posix()] = path
    missing = [name for name, path in files.items() if not path.is_file()]
    if missing:
        raise SystemExit('Missing: ' + ', '.join(missing[:10]))
    running = [p for p in json.loads(call(162, {}))['procs'] if p.get('title_id') == TITLE]
    if running:
        raise SystemExit(f'{TITLE} is running; close it first')

    def connect():
        ftp = ftplib.FTP()
        ftp.connect(HOST, 2121, 300)
        ftp.login()
        ftp.voidcmd('TYPE I')
        return ftp

    ftp = connect()
    listings = {}

    def remote_size(name):
        directory, _, base = (REMOTE + '/' + name).rpartition('/')
        if directory not in listings:
            listings[directory] = remote_sizes(ftp, directory)
        return listings[directory].get(base)

    pending = [(name, path) for name, path in files.items()
               if remote_size(name) != path.stat().st_size or name in ('eboot.bin', 'game/forza.module')]
    total = sum(path.stat().st_size for _, path in pending)
    print(f'{len(pending)} of {len(files)} files to upload ({total / 1e9:.2f} GB)', flush=True)
    known = set()
    sent = 0
    for index, (name, path) in enumerate(pending, 1):
        target = f'{REMOTE}/{name}'
        for attempt in range(3):
            try:
                ensure_dir(ftp, target.rsplit('/', 1)[0], known)
                with path.open('rb') as stream:
                    ftp.storbinary(f'STOR {target}', stream, 1 << 20)
                break
            except (OSError, EOFError, ftplib.Error) as error:
                # The console can be slow to confirm large files; check what arrived.
                print(f'  retry {name}: {error}', flush=True)
                try:
                    ftp.close()
                except OSError:
                    pass
                ftp = connect()
                listings.pop(target.rsplit('/', 1)[0], None)
                if remote_size(name) == path.stat().st_size:
                    break
        else:
            raise SystemExit(f'Upload failed: {name}')
        sent += path.stat().st_size
        if index % 50 == 0 or path.stat().st_size > 50_000_000:
            print(f'  {index}/{len(pending)} {sent / 1e9:.2f} GB {name}', flush=True)
    ftp.quit()
    for name in ('eboot.bin', 'game/forza.module'):
        if remote_sha256(f'{REMOTE}/{name}') != sha256(files[name]):
            raise SystemExit(f'Hash mismatch after upload: {name}')
    print('eboot.bin and forza.module hashes verified', flush=True)
    print('register:', call(56, {'src_path': REMOTE}).decode(errors='replace')[:300])


if __name__ == '__main__':
    main()
