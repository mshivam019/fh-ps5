#!/usr/bin/env python3
"""Decode the bounded PS5 frame trace and correlate display gaps with recorded stages."""
import argparse
import collections
import json
from pathlib import Path
import struct

RECORD = struct.Struct('<QQQQQI116s')
STAGES = {1: 'gpu_swap', 2: 'slow_swap', 3: 'publish', 4: 'request',
          5: 'acquire', 6: 'display_complete', 7: 'command_starvation',
          8: 'fence_wait', 9: 'swap_submission', 10: 'file_read',
          11: 'ui_begin', 12: 'ui_end', 14: 'flip_submit', 15: 'flip_observed',
          16: 'gpu_register_wait'}


def analyze(data):
    if data[:8] != b'FHTRACE1':
        raise ValueError('Unsupported trace header')
    complete = (len(data) - 8) // RECORD.size
    events = []
    for row in RECORD.iter_unpack(data[8:8 + complete * RECORD.size]):
        timestamp, duration, image, argument, thread, stage, path = row
        events.append(dict(ns=timestamp, duration_ns=duration, id=image,
                           argument=argument, thread=thread, stage=STAGES.get(stage, str(stage)),
                           path=path.split(b'\0', 1)[0].decode(errors='replace')))
    events.sort(key=lambda event: event['ns'])
    if not events:
        return {'events': 0, 'display_gaps': [], 'trailing_bytes': (len(data) - 8) % RECORD.size}
    origin = events[0]['ns']
    publications = {}; acquisitions = {}; flips = {}; last_request = None
    last_display = None; last_image = None; gaps = []; repeated = 0
    slow = [event for event in events if event['duration_ns']]
    for event in events:
        stage = event['stage']; image = event['id']
        if stage == 'publish': publications[image] = event
        elif stage == 'request': last_request = event
        elif stage == 'acquire': acquisitions[image] = (event, last_request)
        elif stage == 'flip_submit': flips[image] = event
        elif stage == 'display_complete':
            if image == last_image:
                repeated += 1
                continue
            if last_display and event['ns'] - last_display['ns'] > 45000000:
                start = last_display['ns']; end = event['ns']
                publication = publications.get(image)
                acquisition, request = acquisitions.get(image, (None, None))
                flip = flips.get(event['argument'])
                def elapsed(a, b):
                    return round((b['ns'] - a['ns']) / 1e6, 3) if a and b else None
                overlaps = [dict(stage=s['stage'], ms=round(s['duration_ns'] / 1e6, 3),
                                 path=s['path'], thread=s['thread'], id=s['id'],
                                 argument=s['argument']) for s in slow
                            if s['ns'] >= start and s['ns'] - s['duration_ns'] <= end]
                gaps.append(dict(at_seconds=round((end-origin)/1e9, 3), image=image,
                                 gap_ms=elapsed(last_display, event),
                                 publish_to_acquire_ms=elapsed(publication, acquisition),
                                 request_to_acquire_ms=elapsed(request, acquisition),
                                 acquire_to_flip_ms=elapsed(acquisition, flip),
                                 flip_to_observation_ms=elapsed(flip, event),
                                 overlapping_spans=overlaps))
            last_display = event; last_image = image
    return dict(events=len(events), seconds=round((events[-1]['ns']-origin)/1e9, 3),
                stages=dict(collections.Counter(event['stage'] for event in events)),
                repeated_completions=repeated, trailing_bytes=(len(data)-8) % RECORD.size,
                slowest_spans=[dict(stage=e['stage'], ms=round(e['duration_ns']/1e6, 3),
                                    at_seconds=round((e['ns']-origin)/1e9, 3), path=e['path'],
                                    id=e['id'], argument=e['argument'])
                               for e in sorted(slow, key=lambda e: e['duration_ns'], reverse=True)[:30]],
                display_gaps=gaps,
                caveat='Overlapping spans show temporal correlation, not causation. Completion timestamps are observations, not hardware scanout timestamps. Only spans >=2 ms are recorded. Capture stops after 10 minutes or 65536 events; inspect kernel trace status for drops/write errors.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    parser.add_argument('--json', type=Path, required=True)
    args = parser.parse_args()
    report = analyze(args.trace.read_bytes())
    args.json.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({key: value for key, value in report.items() if key not in ('display_gaps', 'slowest_spans')}, indent=2))
    print('Display gaps >45 ms:', len(report['display_gaps']))
    for gap in sorted(report['display_gaps'], key=lambda gap: gap['gap_ms'], reverse=True)[:10]:
        print(json.dumps(gap))
    print('Report:', args.json)


if __name__ == '__main__':
    main()
