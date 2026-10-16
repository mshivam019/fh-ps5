#!/usr/bin/env python3
"""Extract the files of an Xbox 360 disc image (XDVDFS game partition)."""
import argparse
from pathlib import Path
import struct
import sys

SECTOR = 2048
MAGIC = b'MICROSOFT*XBOX*MEDIA'
# Game partition offsets: plain XISO, XGD2 and XGD3 full-disc images, XGD1.
PARTITIONS = (0, 0xFD90000, 0x2080000, 0x18300000)


def find_partition(image):
    for offset in PARTITIONS:
        image.seek(offset + 32 * SECTOR)
        if image.read(len(MAGIC)) == MAGIC:
            root_sector, root_size = struct.unpack('<II', image.read(8))
            return offset, root_sector, root_size
    raise ValueError('Not an Xbox 360 disc image (no XDVDFS volume found)')


def read_directory(image, partition, sector, size):
    """Yield (name, sector, size, is_directory) for one directory table."""
    if not size:
        return
    image.seek(partition + sector * SECTOR)
    table = image.read(size)
    if len(table) != size:
        raise ValueError('Truncated directory table')
    pending, seen = [0], set()
    while pending:
        offset = pending.pop()
        if offset in seen or offset + 14 > size:
            continue
        seen.add(offset)
        left, right, start, length, attributes, name_length = struct.unpack_from('<HHIIBB', table, offset)
        if left == 0xFFFF and right == 0xFFFF:
            continue  # sector padding
        name = table[offset + 14:offset + 14 + name_length].decode('ascii', 'strict')
        if not name or '/' in name or '\\' in name or name in ('.', '..'):
            raise ValueError(f'Invalid file name in disc image: {name!r}')
        yield name, start, length, bool(attributes & 0x10)
        for child in (left, right):
            if child and child != 0xFFFF:
                pending.append(child * 4)


def extract(image_path, output):
    total = 0
    with image_path.open('rb') as image:
        partition, root_sector, root_size = find_partition(image)
        pending = [(output, root_sector, root_size)]
        while pending:
            directory, sector, size = pending.pop()
            directory.mkdir(parents=True, exist_ok=True)
            for name, start, length, is_directory in read_directory(image, partition, sector, size):
                target = directory / name
                if is_directory:
                    pending.append((target, start, length))
                    continue
                image.seek(partition + start * SECTOR)
                remaining = length
                with target.open('wb') as out:
                    while remaining:
                        chunk = image.read(min(remaining, 8 << 20))
                        if not chunk:
                            raise ValueError(f'Truncated file in disc image: {name}')
                        out.write(chunk)
                        remaining -= len(chunk)
                total += length
    return total


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if args.output.exists():
        parser.error(f'Output already exists: {args.output}')
    try:
        total = extract(args.image, args.output)
    except (ValueError, OSError, UnicodeDecodeError) as error:
        print(f'Extraction failed: {error}', file=sys.stderr)
        sys.exit(1)
    print(f'Extracted {total / 2**30:.2f} GiB to {args.output}')


if __name__ == '__main__':
    main()
