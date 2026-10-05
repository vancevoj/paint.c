#!/usr/bin/env python3
"""Regenerate the lane L6a decoder fixtures in this directory.

The files are written by independent implementations (Pillow and
ImageMagick) from synthetic patterns, so the decoders are checked against
encoders other than our own. The patterns must match tests/codec/
test_own_common.h. All content is generated here; no third-party images.

Requires Pillow and ImageMagick (convert). Run from any directory:
    python3 tests/codec/data/own/gen_fixtures.py [name-prefix ...]
With prefixes, only the matching files are rewritten.
"""
import os
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image, TiffImagePlugin

W, H = 19, 13
HERE = os.path.dirname(os.path.abspath(__file__))


def pat(kind, w=W, h=H):
    y, x = np.mgrid[0:h, 0:w].astype(np.int64)
    r = (x * 7 + y * 3) & 255
    g = (x * 2 + y * 9) & 255
    b = (x * x + y) & 255
    a = (x + y * 5) & 255
    one = np.full_like(x, 255)
    if kind == 'rgba':
        img = np.stack([r, g, b, a], -1)
    elif kind == 'rgb':
        img = np.stack([r, g, b, one], -1)
    elif kind == 'gray':
        img = np.stack([r, r, r, one], -1)
    elif kind == 'bw':
        v = np.where(r > 128, 255, 0)
        img = np.stack([v, v, v, one], -1)
    elif kind in ('few', 'fewt'):
        i = (x // 3 + (y // 2) * 5) % 13
        img = np.stack([(i * 37 + 11) & 255, (i * 91 + 3) & 255, (i * 53 + 200) & 255, one], -1)
        if kind == 'fewt':
            img[(x + y) % 5 == 0] = 0
    else:
        raise ValueError(kind)
    return img.astype(np.uint8)


ONLY = sys.argv[1:]
SKIP = tempfile.TemporaryDirectory()        # files not selected are written here


def out(name):
    if ONLY and not any(name.startswith(p) for p in ONLY):
        return os.path.join(SKIP.name, name)
    return os.path.join(HERE, name)


def pil(kind, mode, name, **kw):
    img = pat(kind)
    if mode == 'RGBA':
        im = Image.fromarray(img, 'RGBA')
    elif mode == 'RGB':
        im = Image.fromarray(img[..., :3], 'RGB')
    elif mode == 'L':
        im = Image.fromarray(img[..., 0], 'L')
    elif mode == '1':
        im = Image.fromarray(img[..., 0] > 128)
    elif mode == 'P':
        im = Image.fromarray(img[..., :3], 'RGB').quantize(16, dither=Image.Dither.NONE)
    im.save(out(name), **kw)


def magick(kind, args, name, prefix=''):
    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, 'src.png')
        img = pat(kind)
        if kind in ('rgba', 'fewt'):
            Image.fromarray(img, 'RGBA').save(src)
        else:                               # opaque sources carry no alpha channel
            Image.fromarray(img[..., :3], 'RGB').save(src)
        cmd = ['convert', src] + args + [prefix + out(name)]
        subprocess.run(cmd, check=True, stdin=subprocess.DEVNULL)


def main():
    # BMP
    pil('rgb', 'RGB', 'bmp_pil_rgb24.bmp')
    pil('rgba', 'RGBA', 'bmp_pil_rgba32.bmp')
    pil('few', 'P', 'bmp_pil_p8.bmp')
    pil('bw', '1', 'bmp_pil_1bit.bmp')
    pil('gray', 'L', 'bmp_pil_gray.bmp')
    magick('few', ['-type', 'Palette', '-compress', 'RLE'], 'bmp_im_rle8.bmp')
    magick('rgb', [], 'bmp_im_bmp3.bmp', 'BMP3:')
    magick('rgba', [], 'bmp_im_v5.bmp')
    magick('rgb', ['-define', 'bmp:subtype=RGB565'], 'bmp_im_565.bmp')
    magick('rgb', ['-define', 'bmp:subtype=RGB555'], 'bmp_im_555.bmp')
    magick('rgba', ['-define', 'bmp:subtype=ARGB4444'], 'bmp_im_4444.bmp')
    # TGA
    pil('rgba', 'RGBA', 'tga_pil_rgba.tga')
    pil('rgba', 'RGBA', 'tga_pil_rgba_rle.tga', compression='tga_rle')
    pil('rgb', 'RGB', 'tga_pil_rgb.tga')
    pil('gray', 'L', 'tga_pil_gray.tga')
    pil('few', 'P', 'tga_pil_p.tga')
    pil('few', 'P', 'tga_pil_p_rle.tga', compression='tga_rle')
    magick('rgba', ['-compress', 'RLE'], 'tga_im_rle.tga')
    magick('rgb', ['-depth', '5', '-define', 'tga:bits=16'], 'tga_im_16.tga')
    # GIF
    pil('few', 'P', 'gif_pil_p.gif')
    pil('few', 'P', 'gif_pil_interlace.gif', interlace=True)
    magick('few', ['-interlace', 'GIF'], 'gif_im_interlace.gif')
    magick('fewt', ['-colors', '14'], 'gif_im_trans.gif')
    magick('few', ['-repage', '30x20+5+3'], 'gif_im_offset.gif')
    with tempfile.TemporaryDirectory() as td:
        a, b = os.path.join(td, 'a.png'), os.path.join(td, 'b.png')
        Image.fromarray(pat('few'), 'RGBA').save(a)
        Image.fromarray(pat('rgb'), 'RGBA').save(b)
        subprocess.run(['convert', '-delay', '10', a, b, '-loop', '0', out('gif_im_anim.gif')],
                       check=True, stdin=subprocess.DEVNULL)
    # TIFF
    for comp, tag in [(None, 'none'), ('tiff_lzw', 'lzw'), ('tiff_adobe_deflate', 'adobe'),
                      ('packbits', 'packbits'), ('tiff_deflate', 'deflate')]:
        pil('rgba', 'RGBA', 'tif_pil_rgba_%s.tif' % tag, compression=comp)
    pil('rgb', 'RGB', 'tif_pil_rgb_none.tif')
    pil('rgb', 'RGB', 'tif_pil_rgb_lzw.tif', compression='tiff_lzw')
    pil('gray', 'L', 'tif_pil_gray.tif')
    pil('few', 'P', 'tif_pil_p.tif')
    pil('bw', '1', 'tif_pil_1bit.tif')
    pil('bw', '1', 'tif_pil_g4.tif', compression='group4')
    pil('bw', '1', 'tif_pil_ccitt_rle.tif', compression='tiff_ccitt')
    pil('bw', '1', 'tif_pil_g3_1d.tif', compression='group3')
    t4 = TiffImagePlugin.ImageFileDirectory_v2()
    t4[292] = 5                                     # 2D coding, EOLs byte aligned
    pil('bw', '1', 'tif_pil_g3_2d.tif', compression='group3', tiffinfo=t4)
    magick('bw', ['-compress', 'Group4', '-define', 'tiff:fill-order=lsb'],
           'tif_im_g4_lsb.tif')
    magick('bw', ['-compress', 'Fax'], 'tif_im_fax.tif')
    pil('rgb', 'RGB', 'tif_pil_jpeg.tif', compression='jpeg')
    magick('rgba', ['-define', 'tiff:tile-geometry=16x16'], 'tif_im_tiled.tif')
    magick('rgba', ['-interlace', 'plane'], 'tif_im_planar.tif')
    magick('rgba', ['-endian', 'MSB'], 'tif_im_be.tif')
    magick('rgba', ['-endian', 'MSB', '-compress', 'LZW', '-define', 'tiff:predictor=2'],
           'tif_im_be_lzw_pred.tif')
    magick('rgba', ['-depth', '16', '-compress', 'Zip', '-define', 'tiff:predictor=2'],
           'tif_im_16_zip_pred.tif')
    magick('rgba', ['-depth', '16', '-endian', 'MSB', '-define', 'tiff:tile-geometry=16x16',
                    '-compress', 'LZW'], 'tif_im_16_be_tiled.tif')
    magick('rgb', ['-colorspace', 'CMYK'], 'tif_im_cmyk.tif')
    magick('gray', ['-colorspace', 'gray', '-define', 'quantum:polarity=min-is-white'],
           'tif_im_miniswhite.tif')
    magick('rgba', ['-orient', 'RightTop'], 'tif_im_orient6.tif')
    magick('rgba', ['-compress', 'RLE'], 'tif_im_packbits.tif')
    magick('rgba', ['-define', 'tiff:rows-per-strip=5', '-compress', 'LZW'],
           'tif_im_strips_lzw.tif')
    magick('rgba', ['-depth', '32', '-define', 'quantum:format=floating-point'],
           'tif_im_float.tif')
    magick('rgba', ['-depth', '16', '-define', 'quantum:format=floating-point',
                    '-compress', 'Zip', '-define', 'tiff:predictor=3'], 'tif_im_half_pred3.tif')
    magick('rgba', ['-depth', '32', '-define', 'quantum:format=floating-point',
                    '-define', 'tiff:tile-geometry=16x16', '-compress', 'LZW',
                    '-define', 'tiff:predictor=3'],
           'tif_im_float_pred3_tiled.tif')
    # (ImageMagick 7.1.1 writes predictor 3 wrongly with -endian MSB: libtiff
    # cannot read those back either; the hand-built tests cover big endian.)
    magick('rgba', ['-depth', '64', '-define', 'quantum:format=floating-point',
                    '-compress', 'LZW', '-define', 'tiff:predictor=2'],
           'tif_im_double_pred2.tif')
    with tempfile.TemporaryDirectory() as td:
        a, b = os.path.join(td, 'a.png'), os.path.join(td, 'b.png')
        Image.fromarray(pat('rgba'), 'RGBA').save(a)
        Image.fromarray(pat('rgb'), 'RGBA').save(b)
        subprocess.run(['convert', a, b, '-adjoin', out('tif_im_multipage.tif')], check=True,
                       stdin=subprocess.DEVNULL)


if __name__ == '__main__':
    main()
