#!/usr/bin/env python3
"""Checks that a Bloodborne dump's compressed game data is intact.

Every DCX archive (zlib, "DFLT") under dvdroot_ps4 must inflate to exactly the size its header
declares, ending at the end of its zlib stream. A dump whose filesystem was extracted wrongly
breaks files past their first 64 KiB block: the game then fails while loading (for example
"shaderBinarySize ... is not equal to Program size ..." and no first frame).

Usage: python windows/check_dump.py "G:/PS4 dump/CUSA03173"
"""
import os
import struct
import sys
import zlib


def check(path):
    data = open(path, 'rb').read()
    if data[:4] != b'DCX\0' or data[0x28:0x2c] != b'DFLT':
        return None  # another format: not checked
    size = struct.unpack_from('>I', data, 0x1c)[0]
    stream = zlib.decompressobj()
    try:
        out = stream.decompress(data[0x4c:])
    except zlib.error:
        return False
    return stream.eof and len(out) == size


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = os.path.join(sys.argv[1], 'dvdroot_ps4')
    if not os.path.isdir(root):
        sys.exit(f'No dvdroot_ps4 folder in {sys.argv[1]}')
    good, bad = 0, []
    for folder, _, files in os.walk(root):
        for name in sorted(files):
            if name.endswith('.dcx'):
                path = os.path.join(folder, name)
                result = check(path)
                if result:
                    good += 1
                elif result is False:
                    bad.append(os.path.relpath(path, root))
    print(f'{good} archives intact, {len(bad)} damaged')
    for name in bad[:20]:
        print(f'  damaged: {name}')
    if len(bad) > 20:
        print(f'  ... and {len(bad) - 20} more')
    if bad:
        print('The dump is damaged: dump the game again from the console.')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
