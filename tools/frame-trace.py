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
          16: 'gpu_register_wait', 17: 'capture_health', 18: 'capture_config',
          20: 'frame_unclassified', 21: 'frame_dispatch', 22: 'frame_empty_ring',
          23: 'frame_register_wait', 24: 'frame_fence_wait',
          25: 'frame_swap', 26: 'frame_submission', 27: 'frame_primitives',
          28: 'frame_textures', 29: 'frame_targets', 30: 'frame_pipelines',
          31: 'frame_bindings', 32: 'frame_vertex_buffers', 33: 'frame_constants',
          34: 'frame_resolve_readback', 35: 'frame_shader_translation',
          36: 'frame_samplers', 40: 'thread_cpu_time', 41: 'cpu_clock_status'}


def analyze(data, after_seconds=0):
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
    capture_seconds = (events[-1]['ns'] - origin) / 1e9
    health = [event for event in events if event['stage'] == 'capture_health']
    configs = [event for event in events if event['stage'] == 'capture_config']
    category_count = configs[-1]['id'] if configs else 7
    cpu_status = [event for event in events if event['stage'] == 'cpu_clock_status']
    events = [event for event in events if (event['ns'] - origin) / 1e9 >= after_seconds]
    publications = {}; acquisitions = {}; flips = {}; last_request = None
    last_display = None; last_image = None; gaps = []; repeated = 0
    slow = [event for event in events if event['duration_ns'] and
            event['stage'] in ('slow_swap', 'command_starvation', 'fence_wait',
                               'swap_submission', 'file_read', 'gpu_register_wait')]
    frames = {}
    cpu_samples = {}
    for event in events:
        if event['stage'].startswith('frame_'):
            frame = frames.setdefault(event['id'], {})
            frame[event['stage']] = round(event['duration_ns'] / 1e6, 6)
        elif event['stage'] == 'thread_cpu_time':
            cpu_samples[event['id']] = event
    def breakdown(frame_id):
        parts = frames.get(frame_id, {})
        cpu = cpu_samples.get(frame_id)
        total = round(sum(parts.values()), 3)
        return dict(command_frame=frame_id, complete=len(parts) == category_count,
                    total_ms=total, categories_ms=parts,
                    thread_cpu_ms=round(cpu['duration_ns'] / 1e6, 3) if cpu else None,
                    cpu_clock_method=cpu['argument'] if cpu else None)
    publication_frames = {}; current_command_frame = None
    for event in events:
        stage = event['stage']; image = event['id']
        if stage == 'gpu_swap': current_command_frame = image
        elif stage == 'publish':
            publications[image] = event
            publication_frames[image] = current_command_frame
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
                                 command_frame=breakdown(publication_frames.get(image)),
                                 overlapping_spans=overlaps))
            last_display = event; last_image = image
    return dict(events=len(events), seconds=round(capture_seconds, 3),
                analysis_start_seconds=after_seconds,
                dropped_events=health[-1]['id'] if health else None,
                health_samples=len(health),
                frame_category_count=category_count,
                cycle_frequency_hz=configs[-1]['argument'] if configs else None,
                cpu_clock_status=[dict(method=event['id'], error=event['argument'],
                                       thread=event['thread']) for event in cpu_status],
                long_command_frames=[breakdown(frame_id) for frame_id, parts in frames.items()
                                     if sum(parts.values()) > 45],
                stages=dict(collections.Counter(event['stage'] for event in events)),
                repeated_completions=repeated, trailing_bytes=(len(data)-8) % RECORD.size,
                slowest_spans=[dict(stage=e['stage'], ms=round(e['duration_ns']/1e6, 3),
                                    at_seconds=round((e['ns']-origin)/1e9, 3), path=e['path'],
                                    id=e['id'], argument=e['argument'])
                               for e in sorted(slow, key=lambda e: e['duration_ns'], reverse=True)[:30]],
                display_gaps=gaps,
                caveat='Stage wall time uses startup-calibrated ordered cycle reads and includes descheduling. Thread CPU time is reported only if a supported clock succeeds; method 1=thread clock, 2=thread rusage. Frame categories are exclusive totals, not contiguous spans. Residual dispatch includes uninstrumented command work. Overlap is not causation. Completion timestamps are observations, not hardware scanout timestamps. Reusable capture stops after 10 minutes or 64 MiB; check dropped_events and kernel write-error status.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    parser.add_argument('--json', type=Path, required=True)
    parser.add_argument('--after-seconds', type=float, default=0,
                        help='Exclude earlier boot/title events while preserving capture-relative timestamps')
    args = parser.parse_args()
    if args.after_seconds < 0:
        parser.error('--after-seconds must be nonnegative')
    report = analyze(args.trace.read_bytes(), args.after_seconds)
    args.json.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({key: value for key, value in report.items()
                      if key not in ('display_gaps', 'slowest_spans', 'long_command_frames')}, indent=2))
    print('Display gaps >45 ms:', len(report['display_gaps']))
    for gap in sorted(report['display_gaps'], key=lambda gap: gap['gap_ms'], reverse=True)[:10]:
        print(json.dumps(gap))
    print('Report:', args.json)


if __name__ == '__main__':
    main()
