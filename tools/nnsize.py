#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Report firmware and NXE XIP capacity (512-byte pages, 32-byte slot headers)."""
import argparse
from pathlib import Path
import struct

from mknxe import NXE_MAGIC, NXE_VERSION


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', nargs='?', type=Path, default=Path('build/nnnix.bin'))
    parser.add_argument('--programs', type=Path, default=Path('build/bin'))
    parser.add_argument('--group', action='append', default=[], help='comma-separated NXE names sharing XIP')
    args = parser.parse_args()
    size = args.image.stat().st_size
    if not 0 < size <= 128 * 1024:
        parser.error('firmware must fit in 128 KiB APROM')
    base = (size + 511) // 512 * 512
    room = 128 * 1024 - base
    print(f'Firmware: {size} B ({size / 1024:.2f} KiB)')
    print(f'XIP: {room} B ({room / 1024:g} KiB), base 0x{base:05x}')
    slots = {}
    for path in sorted(args.programs.glob('*.nxe')):
        if path.stem == 'lcd':          # retired; an old build artifact may remain
            continue
        data = path.read_bytes()
        if len(data) < 40:
            parser.error(f'{path}: truncated NXE')
        magic, version, _, text = struct.unpack_from('<IHHI', data)
        if magic != NXE_MAGIC or version != NXE_VERSION or not text or 40 + text > len(data):
            parser.error(f'{path}: invalid NXE')
        slots[path.stem] = (32 + text + 511) // 512 * 512
    if not slots:
        parser.error(f'no NXE programs in {args.programs}; build programs first')
    print('NXE slots (header and page alignment included):')
    for name, span in sorted(slots.items(), key=lambda item: (-item[1], item[0])):
        print(f'  {name:<10} {span:6} B {"fits" if span <= room else "exceeds XIP"}')
    print(f'All {len(slots)} programs: {sum(slots.values())} B')
    for group in args.group:
        names = set(group.split(','))
        missing = names - slots.keys()
        if missing:
            parser.error('unknown NXE: ' + ', '.join(sorted(missing)))
        needed = sum(slots[name] for name in names)
        print(f'Group {group}: {needed} B, {"fits" if needed <= room else "exceeds XIP"}')
    print('XIP capacity only; runtime RAM and device ownership also limit concurrency.')


if __name__ == '__main__':
    main()
