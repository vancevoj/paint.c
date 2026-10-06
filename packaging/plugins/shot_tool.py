#!/usr/bin/env python3
"""shot_tool.py - images for plugin screenshots (screenshot.sh). Python 3.7+,
standard library only.

  shot_tool.py sample object|photo <out.png> [<width> <height>]
      a test image: "object" is a blue tile with a ring on a transparent
      layer, near the top left (for object plugins); "photo" is an opaque
      smooth gradient landscape (for color and filter effects)
  shot_tool.py bmp2png <in.bmp> <out.png>
      converts the 24 or 32 bit BMP that paintc's "screenshot" script
      command writes
"""
import math
import struct
import sys
import zlib


def write_png(path, w, h, rows, alpha):
    """rows: bytes per row, RGB or RGBA."""
    raw = b"".join(b"\x00" + r for r in rows)
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6 if alpha else 2, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
                chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def clamp(v):
    return 0 if v < 0 else (255 if v > 255 else int(v + 0.5))


def sample(kind, w, h):
    rows = []
    for y in range(h):
        r = bytearray()
        for x in range(w):
            if kind == "photo":
                u, v = x / (w - 1), y / (h - 1)
                sky = v < 0.55 + 0.08 * math.sin(u * 7.0)
                if sky:
                    t = v / 0.6
                    px = (clamp(90 + 120 * t), clamp(150 + 70 * t), clamp(235 - 20 * t), 255)
                else:
                    t = (v - 0.5) * 2
                    px = (clamp(70 + 60 * u - 30 * t), clamp(140 - 40 * t + 20 * math.sin(u * 30)),
                          clamp(60 - 20 * t), 255)
                sx, sy = x - w * 0.72, y - h * 0.22
                if sx * sx + sy * sy < (h * 0.09) ** 2:
                    px = (255, 236, 160, 255)
            else:
                # rounded tile 220 x 170 at (60, 50), ring and triangle inside
                tx, ty = x - 60, y - 50
                px = (0, 0, 0, 0)
                cx, cy = min(max(tx, 36), 220 - 36), min(max(ty, 36), 170 - 36)
                if 0 <= tx < 220 and 0 <= ty < 170 and (tx - cx) ** 2 + (ty - cy) ** 2 <= 36 ** 2:
                    px = (38, 110, 214, 255)
                    d = math.hypot(tx - 110, ty - 85)
                    if 47 <= d <= 65:
                        px = (255, 214, 64, 255)
                    elif ty >= 55 and ty <= 115 and abs(tx - 110) <= (ty - 55) * 0.53:
                        px = (255, 255, 255, 255)
            r += bytes(px)
        rows.append(bytes(r))
    return rows


def bmp2png(src, dst):
    with open(src, "rb") as f:
        data = f.read()
    if data[:2] != b"BM":
        raise SystemExit("bmp2png: %s is not a BMP" % src)
    off, = struct.unpack_from("<I", data, 10)
    w, h, _planes, bpp = struct.unpack_from("<iiHH", data, 18)
    if bpp not in (24, 32):
        raise SystemExit("bmp2png: %d bits per pixel is not supported" % bpp)
    n = bpp // 8
    stride = (w * n + 3) & ~3
    rows = []
    for y in range(abs(h)):
        sy = abs(h) - 1 - y if h > 0 else y
        row = data[off + sy * stride: off + sy * stride + w * n]
        out = bytearray(w * 3)
        out[0::3] = row[2::n]
        out[1::3] = row[1::n]
        out[2::3] = row[0::n]
        rows.append(bytes(out))
    write_png(dst, w, abs(h), rows, False)


def main(argv):
    if len(argv) >= 4 and argv[1] == "sample" and argv[2] in ("object", "photo"):
        w = int(argv[4]) if len(argv) > 5 else 1000
        h = int(argv[5]) if len(argv) > 5 else 620
        write_png(argv[3], w, h, sample(argv[2], w, h), True)
        return 0
    if len(argv) == 4 and argv[1] == "bmp2png":
        bmp2png(argv[2], argv[3])
        return 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
