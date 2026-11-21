#!/usr/bin/env python3
import struct
import sys


def payload(path):
    data = open(path, 'rb').read()
    if len(data) < 512 or data[:8] != b'DHTB\1\0\0\0':
        raise SystemExit(path + ' is not a DHTB image')
    size = struct.unpack_from('<I', data, 48)[0]
    return data[512:512 + size]


image, sml = sys.argv[1], sys.argv[2]
body = payload(sml)
if body not in open(image, 'rb').read():
    raise SystemExit('SML payload not embedded in ' + image)
print('payload embedded:', len(body), 'bytes')
