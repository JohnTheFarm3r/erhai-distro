#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Write a libcamera software ISP tuning file from a Qualcomm Chromatix file.

The tablet's own odm partition carries the stock camera tuning
(com.qti.tuned.<module>.bin). This reads the colour correction matrices
(cc13_ofe_v2) and the lens shading tables (lsc502_ofe_v2) from it and writes
them in the format of libcamera's Ccm and Lsc algorithms.

Chromatix layout, as far as needed here:
  header   section table at 0x98: count, then (type, offset, size) triples;
           the second entry is the module index, type 3 the blob table and
           type 1 the data section
  index    60 byte entries: name[32], version (2 x u16), u32, u32 mode,
           u32 offset and u32 size of the module header in the data section,
           two u32 ordinals
  blobs    16 byte entries: id, offset, size, id of the blob holding the
           data (identical blobs are stored once)
  triggers lists of 24 byte records: f32 start, f32 end, u32 children,
           u32 child list blob, u32 leaf, u32 data blob

The trigger levels are zoom, gain, LED flash, lux index and colour
temperature. The tool takes the first zoom and gain region, no flash, the
lux index region nearest to normal indoor light and every colour
temperature region of it.
"""

import argparse
import re
import struct
import sys

LUX_INDEX = 300          # normal indoor light
LSC_GRID = 17            # libcamera DebayerParams::kLscGridSize
LSC_MAX = 255            # UQ2.6, 64 == 1.0


class Chromatix:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        if not self.d.startswith(b'QTI Chromatix Header'):
            sys.exit(f'{path}: not a Chromatix file')
        d = self.d
        n = struct.unpack_from('<I', d, 0x98)[0]
        tri = [struct.unpack_from('<III', d, 0x9c + i * 12) for i in range(n)]
        secs = {t: (o, s) for t, o, s in tri if t}
        self.data = secs[1][0]
        bo, bs = secs[3]
        self.blobs = {}
        for i in range(bs // 16):
            r = struct.unpack_from('<IIII', d, bo + i * 16)
            self.blobs[r[0]] = r
        io, isz = tri[1][1], tri[1][2]
        self.modules = []
        for k in range((isz - 4) // 60):
            o = io + 4 + k * 60
            name = d[o:o + 32].split(b'\0')[0].decode('ascii', 'replace')
            if not re.fullmatch(r'[a-z][a-z0-9_]{2,31}', name):
                continue
            _, _, _, _, off, size, _, _ = struct.unpack_from('<HHIIIIII', d, o + 32)
            hdr = d[self.data + off:self.data + off + min(size, 4096)]
            ids = [x for x in struct.unpack('<%dI' % (len(hdr) // 4), hdr[:len(hdr) // 4 * 4])
                   if x > 100 and x in self.blobs]
            self.modules.append((name, min(ids) if ids else None))
        firsts = sorted({m[1] for m in self.modules if m[1]})
        self.ranges = {}
        for name, first in self.modules:
            if first is None or name in self.ranges:
                continue
            i = firsts.index(first)
            last = firsts[i + 1] if i + 1 < len(firsts) else max(self.blobs) + 1
            self.ranges[name] = (first, last)

    def blob(self, i):
        r = self.blobs.get(i)
        if not r:
            return b''
        c = self.blobs[r[3]]
        return self.d[self.data + c[1]:self.data + c[1] + c[2]]

    def records(self, i):
        b = self.blob(i)
        if not b or len(b) % 24:
            return None
        out = []
        for k in range(len(b) // 24):
            s, e = struct.unpack_from('<ff', b, k * 24)
            n, child, leaf, data = struct.unpack_from('<IIII', b, k * 24 + 8)
            if not 0 <= s <= e < 1e6 or n > 64 or leaf > 1 or \
               child not in self.blobs or data not in self.blobs:
                return None
            out.append((s, e, n, child, leaf, data))
        return out

    def leaves(self, module):
        """Return [(path of (start, end) per level, data blob id)]."""
        if module not in self.ranges:
            sys.exit(f'module {module} not found')
        lo, hi = self.ranges[module]
        lists = [i for i in range(lo, hi) if self.records(i)]
        referenced = {r[3] for i in lists for r in self.records(i) if r[2]}
        roots = [i for i in lists if i not in referenced]
        if not roots:
            sys.exit(f'{module}: no trigger tree')
        acc = []

        def walk(i, path, depth):
            for s, e, n, child, leaf, data in self.records(i) or []:
                p = path + [(s, e)]
                if n and depth < 8:
                    walk(child, p, depth + 1)
                elif leaf:
                    acc.append((p, data))
        walk(roots[0], [], 0)
        return acc


def select(leaves):
    """Zoom and gain region 0, no flash, lux nearest LUX_INDEX, all CCTs."""
    if not leaves or len({len(l[0]) for l in leaves}) != 1 or len(leaves[0][0]) < 3:
        sys.exit('unexpected trigger tree')
    cand = leaves
    for level in range(len(leaves[0][0]) - 2):
        regions = {l[0][level] for l in cand}
        if all(s == e for s, e in regions):
            # LED flash index: 0 is no flash.
            pick = (0.0, 0.0)
        else:
            pick = min(regions)
        cand = [l for l in cand if l[0][level] == pick]
        if not cand:
            sys.exit(f'no region {pick} at trigger level {level}')

    def dist(r):
        s, e = r
        return 0 if s <= LUX_INDEX <= e else min(abs(s - LUX_INDEX), abs(e - LUX_INDEX))
    lux = min({l[0][-2] for l in cand}, key=dist)
    sets = sorted([l for l in cand if l[0][-2] == lux], key=lambda l: l[0][-1])
    return lux, [(l[0][-1], l[1]) for l in sets]


def ct_of(rng, first, last):
    """Representative colour temperature of a trigger region."""
    s, e = rng
    if first and last:
        return 5000
    if first and s <= 1:
        return int(round(e - 150))
    if last and e >= 9000:
        return int(round(min((s + e) / 2, s + 700)))
    return int(round((s + e) / 2))


def ccms(cx):
    lux, sets = select(cx.leaves('cc13_ofe_v2'))
    out = []
    for k, (rng, data) in enumerate(sets):
        b = cx.blob(data)
        m = struct.unpack_from('<9f', b)
        rows = [m[0:3], m[3:6], m[6:9]]
        if any(abs(sum(r) - 1) > 0.05 for r in rows):
            sys.exit(f'colour matrix {data} does not preserve white')
        out.append((ct_of(rng, k == 0, k == len(sets) - 1), m))
    return lux, out


def resample(grid, rows, cols):
    """Bilinear resample of a corner aligned mesh to LSC_GRID x LSC_GRID."""
    out = []
    for y in range(LSC_GRID):
        fy = y * (rows - 1) / (LSC_GRID - 1)
        y0 = min(int(fy), rows - 2)
        ty = fy - y0
        for x in range(LSC_GRID):
            fx = x * (cols - 1) / (LSC_GRID - 1)
            x0 = min(int(fx), cols - 2)
            tx = fx - x0

            def g(r, c):
                return grid[r * cols + c]
            v = (g(y0, x0) * (1 - tx) * (1 - ty) + g(y0, x0 + 1) * tx * (1 - ty) +
                 g(y0 + 1, x0) * (1 - tx) * ty + g(y0 + 1, x0 + 1) * tx * ty)
            out.append(v)
    return out


def lscs(cx):
    lux, sets = select(cx.leaves('lsc502_ofe_v2'))
    rows, cols = 13, 17
    n = rows * cols
    out = []
    for k, (rng, data) in enumerate(sets):
        b = cx.blob(data)
        if len(b) < 4 * n * 4:
            sys.exit(f'lens shading table {data} too short')
        f = struct.unpack_from('<%df' % (4 * n), b)
        r, gr, gb, bl = (f[i * n:(i + 1) * n] for i in range(4))
        if not all(0.5 < v < 10 for v in f):
            sys.exit(f'lens shading table {data} out of range')
        g = [(a + c) / 2 for a, c in zip(gr, gb)]
        q = {}
        for key, ch in (('r', r), ('g', g), ('b', bl)):
            q[key] = [min(LSC_MAX, max(0, round(v * 64))) for v in resample(ch, rows, cols)]
        out.append((ct_of(rng, k == 0, k == len(sets) - 1), q))
    return lux, out


def fmt_table(vals, indent):
    lines = []
    for y in range(LSC_GRID):
        row = vals[y * LSC_GRID:(y + 1) * LSC_GRID]
        lines.append(indent + ', '.join(f'{v:3d}' for v in row) + ',')
    lines[-1] = lines[-1][:-1]
    return '\n'.join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('chromatix')
    ap.add_argument('output')
    ap.add_argument('--black-level', type=int, default=4096,
                    help='black level in 16 bit units')
    ap.add_argument('--af', action='store_true', help='enable autofocus')
    a = ap.parse_args()

    cx = Chromatix(a.chromatix)
    ccm_lux, cc = ccms(cx)
    lsc_lux, ls = lscs(cx)

    o = ['# SPDX-License-Identifier: CC0-1.0',
         f'# Generated by erhai-distro tools/chromatix-tuning.py from {a.chromatix.split("/")[-1]}:',
         f'# colour matrices for lux index {ccm_lux[0]:g}-{ccm_lux[1]:g},',
         f'# lens shading for lux index {lsc_lux[0]:g}-{lsc_lux[1]:g}.',
         '%YAML 1.1', '---', 'version: 1', 'algorithms:',
         '  - BlackLevel:', f'      blackLevel: {a.black_level}']
    if a.af:
        # Contrast scan over the lens range; statistics arrive every
        # second frame, so keep the number of steps small. Rescan only
        # after six out of focus statistics (0.4 s at 30 fps); skip the first
        # four while exposure settles.
        o += ['  - Af:', '      coarseStep: 64', '      fineStep: 8',
              '      fineRange: 0.1', '      ignoreFrames: 4',
              '      rescanDelay: 6']
    o += ['  - Awb:', '  - Lsc:', '      sets:']
    for ct, q in ls:
        o.append(f'        - ct: {ct}')
        for key in ('r', 'g', 'b'):
            o.append(f'          {key}: [')
            o.append(fmt_table(q[key], '              '))
            o.append('            ]')
    o += ['  - Ccm:', '      ccms:']
    for ct, m in cc:
        o.append(f'        - ct: {ct}')
        o.append('          ccm: [ ' + ', '.join(f'{v:.4f}' for v in m[0:3]) + ',')
        o.append('                 ' + ', '.join(f'{v:.4f}' for v in m[3:6]) + ',')
        o.append('                 ' + ', '.join(f'{v:.4f}' for v in m[6:9]) + ' ]')
    o += ['  - Adjust:', '  - Agc:', '...', '']
    open(a.output, 'w').write('\n'.join(o))
    print(f'{a.output}: {len(cc)} colour matrices, {len(ls)} shading tables')


if __name__ == '__main__':
    main()
