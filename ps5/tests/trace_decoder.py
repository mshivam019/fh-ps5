#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('frame_trace', ROOT / 'tools/frame-trace.py')
trace = importlib.util.module_from_spec(spec)
spec.loader.exec_module(trace)


def event(ms, stage, identity=0, argument=0, duration_ms=0):
    return trace.RECORD.pack(int(ms * 1e6), int(duration_ms * 1e6), identity,
                             argument, 1, stage, b'asset.bin')


class TraceDecoderTests(unittest.TestCase):
    def test_frame_totals_are_not_overlapping_slow_spans(self):
        data = b'FHTRACE1' + event(1, 6, 1, 1)
        totals = [1, 41, 2, 10, 3, 4, 5]
        for category, duration in enumerate(totals):
            data += event(60, 20 + category, 101, duration_ms=duration)
        data += event(60.1, 1, 101) + event(61, 3, 2)
        data += event(62, 4, 2) + event(63, 5, 2)
        data += event(64, 14, 2) + event(65, 10, duration_ms=5)
        data += event(68, 6, 2, 2) + event(70, 17, 7, 22)
        report = trace.analyze(data + b'partial')
        gap = report['display_gaps'][0]
        self.assertEqual(report['dropped_events'], 7)
        self.assertEqual(report['trailing_bytes'], 7)
        self.assertEqual(gap['command_frame']['total_ms'], 66)
        self.assertTrue(gap['command_frame']['complete'])
        self.assertEqual(gap['command_frame']['categories_ms']['frame_dispatch'], 41)
        self.assertEqual([span['stage'] for span in gap['overlapping_spans']], ['file_read'])
        self.assertEqual(gap['request_to_acquire_ms'], 1)
        self.assertEqual(gap['flip_to_observation_ms'], 4)

    def test_race_cutoff_excludes_startup_without_moving_timestamps(self):
        data = b'FHTRACE1' + event(0, 6, 1) + event(200, 6, 2)
        data += event(1000, 6, 3) + event(1050, 6, 4) + event(1060, 17, 11)
        report = trace.analyze(data, after_seconds=1)
        self.assertEqual(len(report['display_gaps']), 1)
        self.assertEqual(report['display_gaps'][0]['at_seconds'], 1.05)
        self.assertEqual(report['display_gaps'][0]['gap_ms'], 50)
        self.assertEqual(report['dropped_events'], 11)
        self.assertEqual(trace.analyze(data, after_seconds=5)['display_gaps'], [])

    def test_dropped_frame_category_is_not_reported_complete(self):
        data = b'FHTRACE1' + event(0, 20, 8, duration_ms=2)
        data += event(10, 21, 8, duration_ms=50)
        report = trace.analyze(data)
        self.assertFalse(report['long_command_frames'][0]['complete'])
        self.assertEqual(report['long_command_frames'][0]['total_ms'], 52)


if __name__ == '__main__':
    unittest.main()
