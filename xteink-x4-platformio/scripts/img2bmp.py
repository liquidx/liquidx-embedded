#!/usr/bin/env python3
"""Convert PNG/JPG images into 1-bit BMPs sized for the X4 screen.

    python3 scripts/img2bmp.py photo.jpg [more.png ...] [-o out_dir]

Each image is scaled and centre-cropped to fill 800x480 (the whole landscape
screen), converted to grayscale and dithered to pure black and white. The
result is a 1 bpp BMP that ImageApp draws pixel for pixel, with no scaling or
dithering on the device. With the chrome shown the card is narrower (716 px),
and the device crops the right-hand edge: it never scales, and pins images
larger than the card to the top-left.

Copy the output to /images on the SD card. Needs Pillow.
"""
import argparse
import sys
from pathlib import Path

from PIL import Image, ImageEnhance, ImageOps

SCREEN_W, SCREEN_H = 800, 480


def parse_size(value):
    try:
        w, h = (int(v) for v in value.lower().split("x"))
    except ValueError:
        raise argparse.ArgumentTypeError(f"expected WxH, got {value!r}")
    return w, h


def convert(src, size, fit, dither, threshold, contrast, brightness, autocontrast):
    img = ImageOps.exif_transpose(Image.open(src))
    # Flatten transparency onto white, the e-ink background.
    if img.mode in ("RGBA", "LA", "PA") or (img.mode == "P" and "transparency" in img.info):
        img = img.convert("RGBA")
        bg = Image.new("RGBA", img.size, "white")
        img = Image.alpha_composite(bg, img)
    img = img.convert("L")

    if fit == "cover":
        img = ImageOps.fit(img, size, Image.LANCZOS)
    else:
        img = ImageOps.pad(img, size, Image.LANCZOS, color=255)

    if autocontrast:
        img = ImageOps.autocontrast(img, cutoff=1)
    if contrast != 1.0:
        img = ImageEnhance.Contrast(img).enhance(contrast)
    if brightness != 1.0:
        img = ImageEnhance.Brightness(img).enhance(brightness)

    if dither == "none":
        return img.point(lambda v: 255 if v >= threshold else 0, "1")
    return img.convert("1", dither=Image.Dither.FLOYDSTEINBERG)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="+", type=Path, help="PNG or JPG files")
    ap.add_argument("-o", "--out", type=Path, help="output directory (default: next to each input)")
    ap.add_argument("--size", type=parse_size, default=(SCREEN_W, SCREEN_H),
                    help=f"output WxH (default {SCREEN_W}x{SCREEN_H})")
    ap.add_argument("--fit", choices=("cover", "contain"), default="cover",
                    help="cover: fill and crop (default); contain: whole image, white bars")
    ap.add_argument("--dither", choices=("fs", "none"), default="fs",
                    help="fs: Floyd-Steinberg (photos, default); none: hard threshold (line art, text)")
    ap.add_argument("--threshold", type=int, default=128, help="cut-off for --dither none (0-255)")
    ap.add_argument("--contrast", type=float, default=1.0, help="contrast factor, e.g. 1.2")
    ap.add_argument("--brightness", type=float, default=1.0, help="brightness factor, e.g. 1.1")
    ap.add_argument("--autocontrast", action="store_true", help="stretch levels before dithering")
    ap.add_argument("--preview", action="store_true", help="also write a .preview.png of the result")
    args = ap.parse_args()

    if args.out:
        args.out.mkdir(parents=True, exist_ok=True)

    failed = 0
    for src in args.inputs:
        dest = (args.out or src.parent) / (src.stem + ".bmp")
        if dest.resolve() == src.resolve():
            print(f"skip {src}: output would overwrite the input", file=sys.stderr)
            failed += 1
            continue
        try:
            img = convert(src, args.size, args.fit, args.dither, args.threshold,
                          args.contrast, args.brightness, args.autocontrast)
        except OSError as e:
            print(f"skip {src}: {e}", file=sys.stderr)
            failed += 1
            continue
        img.save(dest, "BMP")
        if args.preview:
            img.save(dest.with_suffix(".preview.png"))
        print(f"{src} -> {dest} ({img.width}x{img.height}, 1 bpp)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
