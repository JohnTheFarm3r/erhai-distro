#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""ath12k board-2.bin: 'dump FILE' or 'make OUT BOARD REGDB NAME...' (format per ath12k core.c)."""
import struct, sys
MAGIC = b'QCA-ATH12K-BOARD\0'
def pad(b): return b + b'\0' * (-len(b) % 4)
def ie(i, d): return struct.pack('<II', i, len(d)) + pad(d)
def parse(d):
    out = []
    while d:
        i, n = struct.unpack_from('<II', d); out.append((i, d[8:8 + n])); d = d[8 + (n + 3) // 4 * 4:]
    return out
if sys.argv[1] == 'dump':
    d = open(sys.argv[2], 'rb').read(); assert d.startswith(MAGIC)
    for i, body in parse(d[(len(MAGIC) + 3) // 4 * 4:]):
        subs = parse(body)
        print(['BOARD', 'REGDB'][i] if i < 2 else i, [s.decode() for t, s in subs if t == 0], [len(s) for t, s in subs if t == 1])
else:
    out, board, regdb, names = sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5:]
    b, r = open(board, 'rb').read(), open(regdb, 'rb').read()
    blob = pad(MAGIC)
    for kind, data in ((0, b), (1, r)):
        blob += ie(kind, b''.join(ie(0, n.encode()) for n in names) + ie(1, data))
    open(out, 'wb').write(blob)
