#!/usr/bin/env python3
"""Make Android sparse using F2FS's current SIT allocation map.

Keep all metadata and live blocks, coalesce small gaps to reduce bootloader
write calls, and skip unallocated main-area blocks. No mount or phone access.
"""
import argparse
import json
from pathlib import Path
import re
import struct

RAW = 0xcac1
SKIP = 0xcac3
BLOCK = 4096


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('image', type=Path)
    parser.add_argument('sit_dump', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--dirty-check', type=Path, required=True)
    args = parser.parse_args()
    size = args.image.stat().st_size
    assert size % BLOCK == 0
    with args.image.open('rb') as src:
        src.seek(1024)
        sb = src.read(108)
    assert struct.unpack_from('<I', sb)[0] == 0xf2f52010
    segment_blocks = 1 << struct.unpack_from('<I', sb, 20)[0]
    segment_count = struct.unpack_from('<I', sb, 68)[0]
    main = struct.unpack_from('<I', sb, 92)[0]
    blocks = size // BLOCK
    assert main + segment_blocks * segment_count <= blocks
    keep = bytearray(blocks)
    keep[:main] = b'\1' * main
    pieces = re.split(r'segno:\s*(\d+)\s+vblocks:\s*(\d+)[^\n]*\n', args.sit_dump.read_text())
    seen = set()
    live = 0
    for index in range(1, len(pieces), 3):
        segment, count = int(pieces[index]), int(pieces[index + 1])
        assert segment not in seen and segment < segment_count
        seen.add(segment)
        if not count:
            continue
        bitmap = bytes(int(x, 16) for x in re.findall(r'\b[0-9a-f]{2}\b', pieces[index + 2]))
        assert len(bitmap) == segment_blocks // 8, (segment, len(bitmap))
        assert sum(x.bit_count() for x in bitmap) == count
        for bit in range(segment_blocks):
            if bitmap[bit // 8] & (1 << (7 - bit % 8)):
                keep[main + segment * segment_blocks + bit] = 1
        live += count
    assert len(seen) == segment_count
    # Preserve any tail outside the segment area rather than guessing its use.
    tail = main + segment_count * segment_blocks
    keep[tail:] = b'\1' * (blocks - tail)
    cursor = main
    gap_blocks = 32
    while cursor < blocks:
        if keep[cursor]:
            cursor += 1
            continue
        end = cursor + 1
        while end < blocks and not keep[end]:
            end += 1
        if cursor and keep[cursor - 1] and end < blocks and end - cursor <= gap_blocks:
            keep[cursor:end] = b'\1' * (end - cursor)
        cursor = end
    runs = []
    cursor = 0
    max_raw = 32 * 1024 * 1024 // BLOCK
    while cursor < blocks:
        end = cursor + 1
        while end < blocks and keep[end] == keep[cursor]:
            end += 1
        if keep[cursor]:
            for start in range(cursor, end, max_raw):
                runs.append((RAW, start, min(end - start, max_raw)))
        else:
            runs.append((SKIP, cursor, end - cursor))
        cursor = end
    written = 0
    with args.image.open('rb') as src, args.output.open('wb') as dst, args.dirty_check.open('wb') as check:
        dst.write(struct.pack('<IHHHHIIII', 0xed26ff3a, 1, 0, 28, 12, BLOCK, blocks, len(runs), 0))
        garbage = b'\xa5' * (4 * 1024 * 1024)
        for kind, start, length in runs:
            nbytes = length * BLOCK
            dst.write(struct.pack('<HHII', kind, 0, length, 12 + (nbytes if kind == RAW else 0)))
            if kind == RAW:
                src.seek(start * BLOCK)
                payload = src.read(nbytes)
                assert len(payload) == nbytes
                dst.write(payload)
                check.write(payload)
                written += nbytes
            else:
                left = nbytes
                while left:
                    piece = min(left, len(garbage))
                    check.write(garbage[:piece])
                    left -= piece
    assert args.dirty_check.stat().st_size == size
    print(json.dumps({'logical_bytes': size, 'live_blocks': live,
                      'metadata_blocks': main, 'chunks': len(runs),
                      'write_MiB': round(written / 1048576, 1),
                      'skip_MiB': round((size - written) / 1048576, 1),
                      'file_bytes': args.output.stat().st_size,
                      'check':'skipped blocks filled with 0xa5 rather than zero'}, indent=2))


if __name__ == '__main__':
    main()
