#!/usr/bin/env python3
"""Developer console helper for PPSA99690.
  console.py boot TAG [--seconds N]  launch, record the kernel log while it runs, fetch the runtime log
  console.py close                   close Forza only
  console.py log TAG                 fetch the newest runtime log
Output goes to build/console/TAG-*."""
import argparse
import ftplib
import json
import os
from pathlib import Path
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
TITLE = 'PPSA99690'
REMOTE = f'/mnt/ext1/etaHEN/games/{TITLE}'
HOST = os.environ.setdefault('PS5_HOST', '192.168.50.6')
OUT = ROOT / 'build/console'
sys.path.insert(0, str(ROOT.parent / 'Wind-Waker-HD-PS5/ps5/tools'))
from console_control import call  # noqa: E402


def forza_procs():
    return [p for p in json.loads(call(162, {}))['procs'] if p.get('title_id') == TITLE]


def close():
    for proc in forza_procs():
        result = json.loads(call(104, {'action': 'kill', 'app_id': proc['app_id']}))
        if not result.get('ok'):
            raise SystemExit(f'Close failed: {result}')
    for _ in range(15):
        if not forza_procs():
            print('Forza closed')
            return
        time.sleep(1)
    raise SystemExit('Forza still running')


def fetch_log(tag):
    entries = json.loads(call(36, {'path': REMOTE + '/logs'}))['entries']
    logs = [e for e in entries if e['name'].endswith('.log')]
    if not logs:
        print('No runtime log yet')
        return
    newest = max(logs, key=lambda e: (e['mtime'], e['name']))
    path = OUT / f'{tag}-runtime.log'
    ftp = ftplib.FTP()
    ftp.connect(HOST, 2121, 30)
    ftp.login()
    with path.open('wb') as out:
        ftp.retrbinary(f'RETR {REMOTE}/logs/{newest["name"]}', out.write)
    ftp.quit()
    print(path, newest['name'], path.stat().st_size)


def boot(tag, seconds):
    if [p for p in json.loads(call(162, {}))['procs'] if p.get('title_id', '').startswith('PPSA')]:
        raise SystemExit('A game is running; close it first')
    while len(call(108, {'max_bytes': 65536})) == 65536:  # drain old kernel log
        pass
    stop = threading.Event()
    errors = []

    def record():
        with (OUT / f'{tag}-klog.log').open('wb', buffering=0) as out:
            while not stop.is_set():
                try:
                    out.write(call(108, {'max_bytes': 65536}))
                except Exception as error:  # console busy or gone; keep the partial log
                    errors.append(str(error))
                    return
                time.sleep(0.15)

    thread = threading.Thread(target=record, daemon=True)
    thread.start()
    try:
        print('Launch:', call(60, {'title_id': TITLE}).decode(errors='replace'), flush=True)
        end = time.time() + seconds
        seen = False
        while time.time() < end:
            time.sleep(3)
            running = bool(forza_procs())
            seen |= running
            if seen and not running:
                print('Forza exited', flush=True)
                break
        else:
            print(f'Still running after {seconds} s', flush=True)
    finally:
        stop.set()
        thread.join(5)
    if errors:
        print('Kernel log stopped:', errors[0])
    fetch_log(tag)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('action', choices=('boot', 'close', 'log'))
    parser.add_argument('tag', nargs='?', default='run')
    parser.add_argument('--seconds', type=int, default=60)
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    if args.action == 'boot':
        boot(args.tag, args.seconds)
    elif args.action == 'close':
        close()
    else:
        fetch_log(args.tag)


if __name__ == '__main__':
    main()
