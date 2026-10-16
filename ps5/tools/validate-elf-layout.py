#!/usr/bin/env python3
"""Reject overlapping native load segments before signing or deployment."""
import argparse
from pathlib import Path
import struct


def validate(path):
    data = path.read_bytes()
    if data[:6] != b'\x7fELF\x02\x01' or len(data) < 64:
        raise ValueError('expected little-endian ELF64')
    phoff = struct.unpack_from('<Q', data, 32)[0]
    stride, count = struct.unpack_from('<HH', data, 54)
    if stride < 56 or phoff + stride * count > len(data):
        raise ValueError('invalid program-header bounds')
    loads = []
    for index in range(count):
        kind, flags, offset, address, _, size, memory, alignment = struct.unpack_from(
            '<IIQQQQQQ', data, phoff + index * stride)
        if kind != 1 or not memory:
            continue
        if size > memory or offset + size > len(data):
            raise ValueError(f'LOAD {index}: invalid file/memory bounds')
        loads.append((index, address, address + memory, offset, offset + size))
    for i, first in enumerate(loads):
        for second in loads[i + 1:]:
            if max(first[1], second[1]) < min(first[2], second[2]):
                raise ValueError(f'LOAD {first[0]} and {second[0]} overlap in virtual memory')
            if max(first[3], second[3]) < min(first[4], second[4]):
                raise ValueError(f'LOAD {first[0]} and {second[0]} overlap in the file')
    if not loads:
        raise ValueError('no LOAD segments')
    print(f'{path}: {len(loads)} non-overlapping LOAD segments')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('files', nargs='+', type=Path)
    args = parser.parse_args()
    for path in args.files:
        try:
            validate(path)
        except (ValueError, struct.error) as error:
            parser.exit(1, f'{path}: {error}\n')
