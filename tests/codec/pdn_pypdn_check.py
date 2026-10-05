#!/usr/bin/env python3
"""Cross-check paint.c's .pdn codec against the independent MIT pypdn reader.

Usage: pdn_pypdn_check.py TOOL FILE...
  TOOL is the test_pdn_dump executable (built from tests/codec/test_pdn_dump.c).
For each FILE the script reads it with pypdn and with `TOOL --export` and
compares size, layer count, name, visibility, opacity, blend mode and every
pixel. Needs Python 3 with pypdn and numpy (pip install pypdn). Not run by
CTest; see docs/codecs/pdn.md for the recorded results.
"""
import os
import subprocess
import sys
import tempfile

import numpy as np
import pypdn


def ours(tool, path, tmp):
    prefix = os.path.join(tmp, 'x')
    r = subprocess.run([tool, '--export', path, prefix], capture_output=True, text=True)
    if r.returncode != 0:
        return None, r.stdout + r.stderr
    with open(prefix + '.txt', encoding='utf-8') as f:
        head = f.readline().split()
        w, h, n = int(head[0]), int(head[1]), int(head[2])
        props = []
        for _ in range(n):
            line = f.readline().rstrip('\n')
            mode, opacity, visible, name = line.split(' ', 3) + [''] * (4 - len(line.split(' ', 3)))
            props.append((int(mode), int(opacity), visible == '1', name))
    px = np.fromfile(prefix + '.bin', dtype=np.uint8).reshape((n, h, w, 4))
    return (w, h, props, px), None


def check(tool, path):
    with tempfile.TemporaryDirectory() as tmp:
        mine, err = ours(tool, path, tmp)
        if mine is None:
            return 'OURS-FAIL ' + err.strip()
        try:
            ref = pypdn.read(path)
        except Exception as e:  # pypdn cannot read it: report, not a mismatch
            return 'PYPDN-FAIL %s' % e
        w, h, props, px = mine
        if (w, h) != (ref.width, ref.height) or len(props) != len(ref.layers):
            return 'SIZE %r vs %r' % ((w, h, len(props)), (ref.width, ref.height, len(ref.layers)))
        for i, (L, p) in enumerate(zip(ref.layers, props)):
            mode, opacity, visible, name = p
            if mode != int(L.blendMode) or opacity != L.opacity or visible != bool(L.visible):
                return 'PROPS layer %d %r vs %r' % (i, p, (int(L.blendMode), L.opacity, L.visible))
            ref_name = L.name.encode('utf-8')[:63].decode('utf-8', 'ignore')
            if name != ref_name:
                return 'NAME layer %d %r vs %r' % (i, name, ref_name)
            img = np.asarray(L.image)
            bgra = np.empty_like(img)
            bgra[:, :, 0] = img[:, :, 2]
            bgra[:, :, 1] = img[:, :, 1]
            bgra[:, :, 2] = img[:, :, 0]
            bgra[:, :, 3] = img[:, :, 3]
            if not np.array_equal(bgra, px[i]):
                return 'PIXELS layer %d differ in %d values' % (i, int((bgra != px[i]).sum()))
        return 'OK'


def main():
    tool = sys.argv[1]
    counts = {}
    for path in sys.argv[2:]:
        r = check(tool, path)
        key = r.split(' ')[0]
        counts[key] = counts.get(key, 0) + 1
        print('%s %s' % (r, path))
    print(' '.join('%s %d' % kv for kv in sorted(counts.items())))
    return 0 if set(counts) <= {'OK'} else 1


if __name__ == '__main__':
    sys.exit(main())
