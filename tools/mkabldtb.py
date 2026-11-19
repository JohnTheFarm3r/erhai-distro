#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Make a mainline DTB acceptable to the erhai ABL for `fastboot boot`.

ABL applies the stock dtbo.img overlays to the DTB of the boot image. It
aborts if an overlay __fixups__ symbol is missing from the base __symbols__
(a mainline DTB has none), and it selects the base by root qcom,msm-id /
qcom,board-id. This tool:

* maps every symbol named by the selected stock base __symbols__ and by any
  dtbo entry __fixups__ to one inert node, /erhai-abl/overlay-sink, inside a
  container with status = "disabled", so overlay fragments merge there and
  never become devices;
* copies qcom,msm-id and qcom,board-id from the selected stock base tree.

Inputs come from the device's own backup.

Usage: mkabldtb.py --mainline DTB --vendor-boot IMG --dtbo IMG --base-index N --out DTB
"""
import argparse
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

FDT_MAGIC = 0xD00DFEED
DT_TABLE_MAGIC = 0xD7B7AB1E
SINK_PHANDLE = 0x40000000
CONTAINER = 'erhai-abl'
SINK = f'/{CONTAINER}/overlay-sink'
IDENTITY = ('qcom,msm-id', 'qcom,board-id')


def die(msg):
    sys.exit(f'mkabldtb: {msg}')


def run(*cmd, data=None):
    r = subprocess.run(cmd, input=data, capture_output=True)
    if r.returncode:
        die(f'{cmd[0]} failed: {r.stderr.decode(errors="replace").strip()}')
    return r.stdout


def split_fdts(blob):
    out, i = [], 0
    while (j := blob.find(struct.pack('>I', FDT_MAGIC), i)) >= 0:
        size = struct.unpack_from('>I', blob, j + 4)[0]
        out.append(blob[j:j + size])
        i = j + size
    return out


def vendor_boot_dtb(path):
    """DTB section of a vendor_boot header v3/v4 image."""
    d = path.read_bytes()
    if d[:8] != b'VNDRBOOT':
        die(f'{path}: not a vendor_boot image')
    version, page = struct.unpack_from('<II', d, 8)
    if version not in (3, 4):
        die(f'{path}: unsupported vendor_boot header v{version}')
    ramdisk_size = struct.unpack_from('<I', d, 24)[0]
    dtb_size = struct.unpack_from('<I', d, 2100)[0]
    hdr_size = struct.unpack_from('<I', d, 2096)[0]
    pages = lambda n: (n + page - 1) // page * page
    off = pages(hdr_size) + pages(ramdisk_size)
    return d[off:off + dtb_size]


def dtbo_entries(path):
    d = path.read_bytes()
    magic, _total, _hsz, esz, count, eoff = struct.unpack_from('>6I', d, 0)
    if magic != DT_TABLE_MAGIC:
        die(f'{path}: not a dtbo table image')
    out = []
    for n in range(count):
        size, offset = struct.unpack_from('>II', d, eoff + n * esz)
        out.append(d[offset:offset + size])
    return out


def node_props(dtb, node):
    with tempfile.NamedTemporaryFile(suffix='.dtb') as f:
        f.write(dtb)
        f.flush()
        r = subprocess.run(['fdtget', '-p', f.name, node], capture_output=True, text=True)
        return set(r.stdout.split()) if r.returncode == 0 else set()


def root_cells(dtb, prop):
    with tempfile.NamedTemporaryFile(suffix='.dtb') as f:
        f.write(dtb)
        f.flush()
        r = subprocess.run(['fdtget', '-t', 'x', f.name, '/', prop], capture_output=True, text=True)
        return r.stdout.split() if r.returncode == 0 else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--mainline', type=Path, required=True)
    ap.add_argument('--vendor-boot', type=Path, required=True)
    ap.add_argument('--dtbo', type=Path, required=True)
    ap.add_argument('--base-index', type=int, required=True)
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()

    bases = split_fdts(vendor_boot_dtb(a.vendor_boot))
    if not 0 <= a.base_index < len(bases):
        die(f'base index {a.base_index} out of range (0..{len(bases) - 1})')
    base = bases[a.base_index]
    entries = dtbo_entries(a.dtbo)

    symbols = node_props(base, '/__symbols__')
    for e in entries:
        symbols |= node_props(e, '/__fixups__')
    identity = {p: root_cells(base, p) for p in IDENTITY}
    if not identity['qcom,msm-id']:
        die('stock base has no qcom,msm-id')

    text = run('dtc', '-q', '-I', 'dtb', '-O', 'dts', str(a.mainline)).decode()
    if '__symbols__ {' in text:
        die('mainline DTB already has __symbols__ (built with -@?)')
    if f'phandle = <{SINK_PHANDLE:#x}>' in text:
        die('sink phandle already in use')

    head = '/ {\n'
    root_at = text.index(head) + len(head)
    lines = text[root_at:].split('\n')
    # Drop existing identity properties at root level (one tab of indentation).
    lines = [l for l in lines if not any(l.startswith(f'\t{p} =') for p in IDENTITY)]
    props = ''.join(f'\t{p} = <{" ".join("0x" + c for c in v)}>;\n'
                    for p, v in identity.items() if v)
    body = '\n'.join(lines).rstrip()
    if not body.endswith('};'):
        die('unexpected decompiled layout')
    body = body[:-2]
    sink = (f'\t{CONTAINER} {{\n\t\tstatus = "disabled";\n\n'
            f'\t\toverlay-sink {{\n\t\t\tlinux,phandle = <{SINK_PHANDLE:#x}>;\n'
            f'\t\t\tphandle = <{SINK_PHANDLE:#x}>;\n\t\t}};\n\t}};\n\n'
            '\t__symbols__ {\n'
            + ''.join(f'\t\t{s} = "{SINK}";\n' for s in sorted(symbols))
            + '\t};\n};\n')
    out_text = text[:root_at] + props + body + sink
    a.out.write_bytes(run('dtc', '-q', '-I', 'dts', '-O', 'dtb', '-', data=out_text.encode()))

    final = a.out.read_bytes()
    got = node_props(final, '/__symbols__')
    missing = [n for n, e in enumerate(entries) if not node_props(e, '/__fixups__') <= got]
    if missing:
        die(f'dtbo entries with unresolved fixups: {missing}')
    for p, v in identity.items():
        if v and root_cells(final, p) != v:
            die(f'identity {p} mismatch')
    print(f'base[{a.base_index}] msm-id={" ".join(identity["qcom,msm-id"])} '
          f'board-id={" ".join(identity["qcom,board-id"] or ["-"])} '
          f'symbols={len(got)} dtbo_entries={len(entries)} out={a.out} ({len(final)} bytes)')


if __name__ == '__main__':
    main()
