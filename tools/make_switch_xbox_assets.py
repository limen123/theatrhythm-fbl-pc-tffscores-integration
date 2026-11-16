#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Derive Switch-layout button icon and controller pad assets from existing custom Xbox art.

    python tools/make_switch_xbox_assets.py

Generates:
  - assets/button_icons/switch.png + switch.bc7 (from xbox.png, swapping A<->B and X<->Y)
  - assets/controller_pad/switch.png + switch.bc7 (from xbox.png, swapping Y<->X and B<->A caps)

Uses only the repository's existing custom Xbox PNGs so no external controller art is needed.
Requires Pillow and etcpak (and numpy/scipy-free pure Pillow+numpy).
"""
import json
import subprocess
import sys
from pathlib import Path

import etcpak
import numpy as np
from PIL import Image

PORT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PORT / 'scripts'))
import controller_pad as cp  # noqa: E402


def make_button_icons():
    folder = PORT / 'assets' / 'button_icons'
    rects = {r['name']: r for r in json.loads((folder / 'sprite_rects.json').read_text())}
    im = Image.open(folder / 'xbox.png').convert('RGBA')
    out = im.copy()
    pairs = [
        ('BK_Symbol1', 'BK_Symbol2'),
        ('BK_Symbol3', 'BK_Symbol4'),
        ('WT_Symbol1', 'WT_Symbol2'),
        ('WT_Symbol3', 'WT_Symbol4'),
    ]
    for a_name, b_name in pairs:
        a, b = rects[a_name], rects[b_name]
        if (a['w'], a['h']) != (b['w'], b['h']):
            raise ValueError(f'Size mismatch for {a_name} vs {b_name}')
        box_a = (a['x'], a['y'], a['x'] + a['w'], a['y'] + a['h'])
        box_b = (b['x'], b['y'], b['x'] + b['w'], b['y'] + b['h'])
        crop_a = im.crop(box_a)
        crop_b = im.crop(box_b)
        out.paste(crop_b, (a['x'], a['y']))
        out.paste(crop_a, (b['x'], b['y']))
    switch_png = folder / 'switch.png'
    out.save(switch_png)
    subprocess.run([sys.executable, str(PORT / 'tools' / 'encode_button_icons.py'), str(switch_png)], check=True)


def _sample_bilinear(arr, xs, ys):
    h, w, _ = arr.shape
    x0 = np.floor(xs).astype(int)
    y0 = np.floor(ys).astype(int)
    fx = (xs - x0)[..., None]
    fy = (ys - y0)[..., None]
    x1 = np.clip(x0 + 1, 0, w - 1)
    y1 = np.clip(y0 + 1, 0, h - 1)
    x0 = np.clip(x0, 0, w - 1)
    y0 = np.clip(y0, 0, h - 1)
    top = arr[y0, x0] * (1.0 - fx) + arr[y0, x1] * fx
    bot = arr[y1, x0] * (1.0 - fx) + arr[y1, x1] * fx
    return top * (1.0 - fy) + bot * fy


def _swap_circle_pair(src, dst, c1, c2, r_inner, r_outer):
    cx1, cy1 = c1
    cx2, cy2 = c2
    h, w, _ = src.shape
    for (dcx, dcy), (scx, scy) in (((cx1, cy1), (cx2, cy2)), ((cx2, cy2), (cx1, cy1))):
        x0 = max(0, int(np.floor(dcx - r_outer - 1)))
        x1 = min(w, int(np.ceil(dcx + r_outer + 2)))
        y0 = max(0, int(np.floor(dcy - r_outer - 1)))
        y1 = min(h, int(np.ceil(dcy + r_outer + 2)))
        yy, xx = np.mgrid[y0:y1, x0:x1]
        dx = xx.astype(float) - dcx
        dy = yy.astype(float) - dcy
        dist = np.sqrt(dx * dx + dy * dy)
        alpha = np.clip((r_outer - dist) / (r_outer - r_inner), 0.0, 1.0)[..., None]
        sampled = _sample_bilinear(src, scx + dx, scy + dy)
        dst[y0:y1, x0:x1] = dst[y0:y1, x0:x1] * (1.0 - alpha) + sampled * alpha


def make_controller_pad():
    folder = PORT / 'assets' / 'controller_pad'
    im = Image.open(folder / 'xbox.png').convert('RGBA')
    src = np.array(im, dtype=np.float64)
    dst = src.copy()

    # Refined ring centroids from xbox.png: swap Y<->X and B<->A on both large & small pads
    large = {'Y': (372.89, 321.82), 'B': (403.28, 348.90), 'X': (340.72, 352.77), 'A': (371.27, 380.03)}
    small = {'Y': (167.11, 140.03), 'B': (180.62, 151.99), 'X': (152.92, 153.80), 'A': (166.44, 165.65)}
    _swap_circle_pair(src, dst, large['Y'], large['X'], r_inner=17.8, r_outer=19.2)
    _swap_circle_pair(src, dst, large['B'], large['A'], r_inner=17.8, r_outer=19.2)
    _swap_circle_pair(src, dst, small['Y'], small['X'], r_inner=7.6, r_outer=8.8)
    _swap_circle_pair(src, dst, small['B'], small['A'], r_inner=7.6, r_outer=8.8)

    out_im = Image.fromarray(np.clip(np.round(dst), 0, 255).astype(np.uint8), 'RGBA')
    switch_png = folder / 'switch.png'
    out_im.save(switch_png)

    bottom_up = out_im.transpose(Image.FLIP_TOP_BOTTOM)
    whole_bc7 = etcpak.compress_bc7(bottom_up.tobytes(), out_im.width, out_im.height)
    blocks = bytearray()
    for bx, by, bw, bh in cp.REGIONS:
        for row in range(by, by + bh):
            off = (row * cp.BLOCKS + bx) * cp.BLOCK_BYTES
            blocks.extend(whole_bc7[off : off + bw * cp.BLOCK_BYTES])
    if len(blocks) != cp.PATCH_BYTES:
        raise ValueError(f'Unexpected BC7 patch size: {len(blocks)} != {cp.PATCH_BYTES}')
    (folder / 'switch.bc7').write_bytes(bytes(blocks))
    print(f'Wrote {switch_png} and {folder / "switch.bc7"} ({len(blocks)} bytes)')


def main():
    make_button_icons()
    make_controller_pad()


if __name__ == '__main__':
    main()

