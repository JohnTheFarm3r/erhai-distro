#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Extract one logical partition from a raw (non-sparse) Android super image."""
import struct, sys
src, name, out = sys.argv[1:4]
f = open(src, 'rb')
f.seek(4096); g = f.read(4096)
assert struct.unpack_from('<I', g, 0)[0] == 0x616c4467, 'no LP geometry'
f.seek(12288); h = f.read(4096)
assert struct.unpack_from('<I', h, 0)[0] == 0x414c5030, 'no LP header'
hsize = struct.unpack_from('<I', h, 8)[0]
tsize = struct.unpack_from('<I', h, 44)[0]
po, pn, ps, eo, en, es = struct.unpack_from('<6I', h, 80)
f.seek(12288 + hsize); t = f.read(tsize)
for i in range(pn):
    e = t[po + i * ps: po + (i + 1) * ps]
    pname = e[:36].rstrip(b'\0').decode()
    _attr, first, num, _grp = struct.unpack_from('<4I', e, 36)
    if pname != name:
        continue
    with open(out, 'wb') as o:
        for x in range(first, first + num):
            sectors, ttype, data, _s = struct.unpack_from('<QIQI', t, eo + x * es)
            assert ttype == 0, 'non-linear extent'
            f.seek(data * 512); left = sectors * 512
            while left:
                b = f.read(min(left, 1 << 24)); o.write(b); left -= len(b)
    print(f'{name}: {num} extents -> {out}'); break
else:
    sys.exit(f'{name} not found')
