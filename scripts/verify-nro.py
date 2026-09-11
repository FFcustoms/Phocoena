#!/usr/bin/env python3
"""Structural validation of elf2nro output (not a substitute for hardware)."""
import argparse
import hashlib
import json
import struct
from pathlib import Path


def verify(data):
    if len(data) < 0x80 or data[0x10:0x14] != b'NRO0':
        raise ValueError('not an NRO0 image')
    version, size, flags = struct.unpack_from('<III', data, 0x14)
    if version != 0 or size > len(data) or size < 0x80:
        raise ValueError('invalid NRO version/size')
    segments = []
    last_end = 0
    for name, pos in [('text', 0x20), ('ro', 0x28), ('data', 0x30)]:
        offset, length = struct.unpack_from('<II', data, pos)
        if offset % 0x1000 or offset < last_end or offset + length > size:
            raise ValueError('invalid/overlapping ' + name + ' segment')
        segments.append({'name': name, 'offset': offset, 'size': length})
        last_end = offset + length
    if not segments[0]['size']:
        raise ValueError('empty code segment')
    module_offset = struct.unpack_from('<I', data, 4)[0]
    if module_offset + 28 > size or data[module_offset:module_offset + 4] != b'MOD0':
        raise ValueError('missing MOD0 header')
    if data[size:size + 4] != b'ASET' or len(data) < size + 0x38:
        raise ValueError('missing asset header/NACP')
    asset_version = struct.unpack_from('<I', data, size + 4)[0]
    if asset_version != 0:
        raise ValueError('unsupported asset version')
    nacp_offset, nacp_size = struct.unpack_from('<QQ', data, size + 0x18)
    if nacp_size != 0x4000 or nacp_offset < 0x38 or size + nacp_offset + nacp_size > len(data):
        raise ValueError('invalid NACP range')
    name = data[size + nacp_offset:size + nacp_offset + 0x200].split(b'\0')[0].decode('utf-8')
    display_version = data[size + nacp_offset + 0x3060:size + nacp_offset + 0x3070].split(b'\0')[0].decode('utf-8')
    return {'format': 'NRO0', 'size': len(data), 'load_size': size, 'name': name,
            'version': display_version,
            'segments': segments, 'sha256': hashlib.sha256(data).hexdigest(),
            'hardware_tested': False}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('nro', type=Path)
    args = parser.parse_args()
    print(json.dumps(verify(args.nro.read_bytes()), indent=2))
