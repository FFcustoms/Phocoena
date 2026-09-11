#!/usr/bin/env python3
"""Create an original, minimal GameCube PPC DOL; no Nintendo assets required.

This loops forever, incrementing r3. It does not initialize video/audio; a black
screen is expected. Use core logs and Plus+Minus to check boot/stop behavior.
"""
import argparse
import struct
from pathlib import Path


def validate_dol(data):
    """Check section ranges using DolReader's aligned-read rule, not just raw sizes.

    This is a package integrity check, not an emulator or an execution test.
    """
    if len(data) < 0x100:
        raise ValueError('DOL header is truncated')
    entry = struct.unpack_from('>I', data, 0xE0)[0]
    entry_in_text = False
    for index in range(18):
        offset = struct.unpack_from('>I', data, index * 4)[0]
        address = struct.unpack_from('>I', data, 0x48 + index * 4)[0]
        size = struct.unpack_from('>I', data, 0x90 + index * 4)[0]
        if not size:
            continue
        aligned_size = (size + 31) & ~31
        if offset < 0x100 or offset + aligned_size > len(data):
            raise ValueError(f'DOL section {index} aligned read exceeds file bounds')
        if address + aligned_size > 0x100000000:
            raise ValueError(f'DOL section {index} address overflows')
        if index < 7 and address <= entry < address + size and entry % 4 == 0:
            entry_in_text = True
    if not entry_in_text:
        raise ValueError('DOL entry point is outside its text sections')


def make_dol():
    header = bytearray(0x100)
    code = struct.pack('>III', 0x3860002A, 0x38630001, 0x4BFFFFFC)
    # DolReader reads AlignUp(section_size, 32) bytes, even for a tiny section.
    # Pad with unreachable PPC nops and include them in the section size.
    code += struct.pack('>I', 0x60000000) * 5
    struct.pack_into('>I', header, 0x00, 0x100)      # text[0] file offset
    struct.pack_into('>I', header, 0x48, 0x80003100) # text[0] load address
    struct.pack_into('>I', header, 0x90, len(code))  # text[0] size
    struct.pack_into('>I', header, 0xE0, 0x80003100) # entrypoint
    data = bytes(header) + code
    validate_dol(data)
    return data


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(make_dol())
    print(f'Wrote synthetic PPC smoke test: {args.output}')
