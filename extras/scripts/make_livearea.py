#!/usr/bin/env python3
"""
Builds LiveArea images (icon0, pic0, bg0, startup) from the game's own assets.

Usage: make_livearea.py <extracted-apk-or-assets-dir> <output-dir>

The images contain Lima Sky's artwork, so they are generated locally and not
stored in this repository. Point the LIVEAREA_DIR CMake option at the output.
"""

import os
import shutil
import sys

from PIL import Image

# Regions in gametiles_X.png (1800x1024)
GREEN_PLATFORM = (0, 0, 120, 36)
BLUE_PLATFORM = (0, 36, 120, 70)
FLYING_MONSTER = (296, 0, 452, 92)
GREEN_MONSTER = (128, 476, 292, 584)
PROPELLER = (660, 470, 722, 516)


def load(assets, name):
    return Image.open(os.path.join(assets, name)).convert("RGBA")


def crop_tight(img, box):
    part = img.crop(box)
    bbox = part.getbbox()
    return part.crop(bbox) if bbox else part


def scaled(img, factor):
    w, h = img.size
    return img.resize((max(1, int(w * factor)), max(1, int(h * factor))), Image.LANCZOS)


def paper(size, assets):
    """Tiles the graph paper background over the requested size."""
    tile = load(assets, "bck_X.png")
    out = Image.new("RGBA", size)
    for y in range(0, size[1], tile.size[1]):
        for x in range(0, size[0], tile.size[0]):
            out.paste(tile, (x, y))
    return out


def paste(dst, img, center):
    x = int(center[0] - img.size[0] / 2)
    y = int(center[1] - img.size[1] / 2)
    dst.alpha_composite(img, (x, y))


def save_indexed(img, path):
    # LiveArea images must be 8-bit palettized PNGs.
    img.convert("RGB").quantize(colors=256, method=Image.MEDIANCUT, dither=Image.FLOYDSTEINBERG).save(path, optimize=True)


def scene(size, assets, logo_scale, doodler_scale, with_monsters=True):
    tiles = load(assets, "gametiles_X.png")
    logo = load(assets, "doodlejump_X.png")
    doodler = load(assets, "likright_X.png")
    green = crop_tight(tiles, GREEN_PLATFORM)
    blue = crop_tight(tiles, BLUE_PLATFORM)

    w, h = size
    img = paper(size, assets)
    k = h / 544.0

    # Platforms climbing up to the right, the doodler jumping off one of them
    for cx, cy, plat in [(0.18, 0.86, green), (0.42, 0.70, green), (0.70, 0.88, blue),
                         (0.86, 0.56, green), (0.12, 0.48, green)]:
        paste(img, scaled(plat, 1.1 * k), (w * cx, h * cy))
    paste(img, scaled(doodler, doodler_scale * k), (w * 0.42, h * 0.70 - 75 * doodler_scale * k))

    if with_monsters:
        paste(img, scaled(crop_tight(tiles, FLYING_MONSTER), 0.85 * k), (w * 0.84, h * 0.30))
        paste(img, scaled(crop_tight(tiles, GREEN_MONSTER), 0.7 * k), (w * 0.09, h * 0.36))

    paste(img, scaled(logo, logo_scale * k), (w * 0.5, h * 0.17))
    return img


def make_startup(assets):
    """The gate sits on top of bg0 (which already has the logo): play button instead."""
    tiles = load(assets, "gametiles_X.png")
    doodler = load(assets, "likright_X.png")
    play = load(assets, "play_X.png")
    green = crop_tight(tiles, GREEN_PLATFORM)
    blue = crop_tight(tiles, BLUE_PLATFORM)

    img = paper((280, 158), assets)
    paste(img, scaled(green, 0.55), (46, 128))
    paste(img, scaled(doodler, 0.62), (46, 92))
    paste(img, scaled(blue, 0.45), (238, 44))
    paste(img, scaled(crop_tight(tiles, FLYING_MONSTER), 0.42), (236, 118))
    paste(img, scaled(play, 0.55), (162, 74))
    return img


def make_icon(assets):
    tiles = load(assets, "gametiles_X.png")
    doodler = load(assets, "likright_X.png")
    green = crop_tight(tiles, GREEN_PLATFORM)

    img = paper((128, 128), assets)
    paste(img, scaled(green, 0.85), (64, 108))
    paste(img, scaled(doodler, 0.78), (62, 58))
    return img


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)

    src, out = sys.argv[1], sys.argv[2]
    assets = os.path.join(src, "assets") if os.path.isdir(os.path.join(src, "assets")) else src
    os.makedirs(out, exist_ok=True)

    save_indexed(make_icon(assets), os.path.join(out, "icon0.png"))
    save_indexed(scene((960, 544), assets, 1.6, 1.3), os.path.join(out, "pic0.png"))
    save_indexed(scene((840, 500), assets, 1.5, 1.2), os.path.join(out, "bg0.png"))

    save_indexed(make_startup(assets), os.path.join(out, "startup.png"))

    template = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "livearea", "template.xml")
    shutil.copy(template, os.path.join(out, "template.xml"))
    print("LiveArea images written to", out)


if __name__ == "__main__":
    main()
