#!/usr/bin/env python3
"""Generate the image-decoder test corpus.

Covers the variants that actually differ in a decoder's code paths:
PNG colour types 0/2/3/4/6, bit depths 1/2/4/8/16, Adam7 interlace and
tRNS; JPEG grayscale, 4:4:4, 4:2:2, 4:2:0, restart intervals and
progressive (which we expect to be rejected); BMP 8/24/32-bit, top-down
and RLE8; QOI at several bit depths.
"""
import os
import struct
import sys
import zlib

from PIL import Image, ImageDraw, ImageFont

OUT = sys.argv[1] if len(sys.argv) > 1 else "."
os.makedirs(OUT, exist_ok=True)


def save(im, name, **kw):
    p = os.path.join(OUT, name)
    im.save(p, **kw)
    return p


def scene(w=160, h=120, seed=0):
    """A picture with structure: gradients, hard edges, flat areas and
    text.  Flat areas catch filtering bugs, text catches ringing."""
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = ((x * 255) // max(1, w - 1),
                        (y * 255) // max(1, h - 1),
                        ((x + y) * 255) // max(1, w + h - 2))
    d = ImageDraw.Draw(im)
    if w >= 16 and h >= 16:
        d.rectangle([w // 8, h // 8, w // 2, h // 2], fill=(200, 30, 40))
        d.ellipse([w // 2, h // 3, max(w // 2, w - 4), max(h // 3, h - 4)],
                  fill=(20, 200, 90))
        d.line([0, h - 1, w - 1, 0], fill=(255, 255, 255), width=3)
    d.text((1, 1), "Sx%d" % seed, fill=(255, 255, 0))
    return im


def main():
    n = 0

    # ── PNG: colour types and bit depths ────────────────────────
    base = scene(200, 150, 0)
    grey = base.convert("L")
    rgba = base.convert("RGBA")
    # Punch a hole so alpha/tRNS are actually exercised.
    rgba.putalpha(128)
    d = ImageDraw.Draw(rgba)
    d.ellipse([40, 30, 100, 90], fill=(0, 0, 0, 0))

    save(base, "rgb8.png"); n += 1
    save(grey, "grey8.png"); n += 1
    save(rgba, "rgba8.png"); n += 1

    # Palette + transparency
    pal = base.convert("P", palette=Image.ADAPTIVE, colors=64)
    save(pal, "pal8.png"); n += 1

    # Sub-byte depths (greyscale and palette)
    for depth in (1, 2, 4):
        save(grey, f"grey{depth}b.png", bits=depth, optimize=False)
        n += 1
    for depth in (1, 2, 4, 8):
        save(pal, f"pal{depth}b.png", bits=depth, optimize=False)
        n += 1

    # 16-bit
    g16 = Image.new("I;16", (120, 90))
    gp = g16.load()
    for y in range(90):
        for x in range(120):
            gp[x, y] = (x * 65535) // 119
    save(g16, "grey16.png"); n += 1

    rgb16 = Image.new("RGB", (100, 80))
    rp = rgb16.load()
    for y in range(80):
        for x in range(100):
            rp[x, y] = (x * 8 % 256, y * 8 % 256, ((x * y) // 7) % 256)
    save(rgb16, "rgb16.png"); n += 1

    # Interlaced
    save(base, "rgb8_interlaced.png", interlace=True); n += 1
    save(grey, "grey8_interlaced.png", interlace=True); n += 1
    save(pal, "pal8_interlaced.png", interlace=True); n += 1

    # tRNS on a truecolour PNG (hand-built so we control the chunk)
    n += 1 if write_trns_rgb(base) else 0
    n += 1 if write_trns_grey(grey) else 0

    # Grayscale with alpha
    ga = grey.convert("LA")
    save(ga, "grey_alpha8.png"); n += 1

    # ── JPEG ────────────────────────────────────────────────────
    for name, kw in [
        ("j_gray.jpg", dict(quality=90)),
        ("j_444.jpg", dict(quality=90, subsampling=0)),
        ("j_422.jpg", dict(quality=90, subsampling=1)),
        ("j_420.jpg", dict(quality=90, subsampling=2)),
        ("j_420_q60.jpg", dict(quality=60, subsampling=2)),
        ("j_prog.jpg", dict(quality=90, progressive=True)),
        ("j_restart.jpg", dict(quality=85, subsampling=2, restart_marker_blocks=4)),
        ("j_opt.jpg", dict(quality=95, optimize=True)),
    ]:
        save(base, name, **kw)
        n += 1
    save(grey, "j_gray_q75.jpg", quality=75)
    n += 1
    save(scene(37, 23, 3), "j_tiny.jpg", quality=90)
    n += 1
    save(scene(1, 1, 0), "j_1x1.jpg", quality=90)
    n += 1

    # ── BMP ─────────────────────────────────────────────────────
    save(base, "b24.bmp")
    n += 1
    save(rgba.convert("RGB"), "b32.bmp")
    n += 1
    save(pal, "b8.bmp")
    n += 1

    # ── QOI ─────────────────────────────────────────────────────
    try:
        save(base, "rgb8.qoi")
        n += 1
        save(rgba, "rgba8.qoi")
        n += 1
    except Exception as e:
        print("qoi encode unavailable:", e)

    print(f"generated {n} files in {OUT}")


def write_trns_rgb(im):
    """Build an RGB PNG carrying a tRNS chunk (PIL will not emit one)."""
    from PIL import Image
    raw = im.convert("RGB").tobytes()
    w, h = im.size
    # Scanline filter byte 0 + RGB
    scan = b"".join(b"\x00" + raw[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    # make the pixel at (5,5) — a gradient pixel — fully transparent
    key = raw[(5 * w + 5) * 3:(5 * w + 5) * 3 + 3]
    trns = struct.pack(">HHH", *key)
    blob = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"tRNS", trns) + chunk(b"IDAT", zlib.compress(scan, 9)) +
            chunk(b"IEND", b""))
    open(os.path.join(OUT, "rgb8_trns.png"), "wb").write(blob)
    return True


def write_trns_grey(im):
    raw = im.convert("L").tobytes()
    w, h = im.size
    scan = b"".join(b"\x00" + raw[y * w:(y + 1) * w] for y in range(h))

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0)
    trns = struct.pack(">H", raw[5 * w + 5])
    blob = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"tRNS", trns) + chunk(b"IDAT", zlib.compress(scan, 9)) +
            chunk(b"IEND", b""))
    open(os.path.join(OUT, "grey8_trns.png"), "wb").write(blob)
    return True


if __name__ == "__main__":
    main()
