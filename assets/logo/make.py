#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Draws Hokuto's logo and app icon, and builds the icon files for Windows and macOS.

`make.py` writes icon.svg, logo-light.svg and logo-dark.svg. `make.py --render` also writes hokuto.ico, hokuto.icns
and png/. Rendering requires macOS. It uses Quick Look (qlmanage) to draw the SVG files and iconutil to build the
ICNS file. make.py then strips the metadata that iconutil adds.

The icon draws the seven stars of the Big Dipper as notes on a piano roll. Every shape is drawn here
from coordinates. No font is used.
"""

import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))

INK_LIGHT = "#1c2033"  # wordmark on light backgrounds
INK_DARK = "#f4f1e8"  # wordmark on dark backgrounds

ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 256]
PNG_SIZES = [16, 32, 48, 64, 128, 256, 512]
SMALL = 32  # sizes up to this one use the simpler icon


def f(v):
    s = f"{v:.1f}"
    if s.endswith(".0"):
        s = s[:-2]
    return "0" if s == "-0" else s


# The Big Dipper (J2000): right ascension and declination in degrees.
STARS = {
    "Dubhe": (165.932, 61.751),
    "Merak": (165.460, 56.383),
    "Phecda": (178.457, 53.695),
    "Megrez": (183.857, 57.033),
    "Alioth": (193.507, 55.960),
    "Mizar": (200.981, 54.925),
    "Alkaid": (206.885, 49.313),
}
LINES = [("Alkaid", "Mizar"), ("Mizar", "Alioth"), ("Alioth", "Megrez"), ("Megrez", "Dubhe"),
         ("Dubhe", "Merak"), ("Merak", "Phecda"), ("Phecda", "Megrez")]


def sky(rotate):
    """Returns each star's position in SVG coordinates (y down). The projection is gnomonic, with north up and east
    left, turned anticlockwise by `rotate` degrees."""
    a0, d0 = math.radians(187.0), math.radians(55.5)
    r = math.radians(rotate)
    c, s = math.cos(r), math.sin(r)
    out = {}
    for name, (ra, dec) in STARS.items():
        a, d = math.radians(ra), math.radians(dec)
        cosc = math.sin(d0) * math.sin(d) + math.cos(d0) * math.cos(d) * math.cos(a - a0)
        x = -math.cos(d) * math.sin(a - a0) / cosc
        y = (math.cos(d0) * math.sin(d) - math.sin(d0) * math.cos(d) * math.cos(a - a0)) / cosc
        out[name] = (x * c - y * s, -(x * s + y * c))
    return out


def fit(points, box, stretch=1.0):
    """Scales and centres the stars in box (x, y, w, h). The shape stays the same, apart from a vertical stretch."""
    xs = [x for x, _ in points.values()]
    ys = [y * stretch for _, y in points.values()]
    bx, by, bw, bh = box
    k = min(bw / (max(xs) - min(xs)), bh / (max(ys) - min(ys)))
    ox = bx + (bw - (max(xs) - min(xs)) * k) / 2 - min(xs) * k
    oy = by + (bh - (max(ys) - min(ys)) * k) / 2 - min(ys) * k
    return {n: (x * k + ox, y * stretch * k + oy) for n, (x, y) in points.items()}


def squircle(size=1024.0, n=5.0, steps=240):
    r = size / 2
    p = []
    for i in range(steps):
        t = 2 * math.pi * i / steps
        c, s = math.cos(t), math.sin(t)
        p.append((r + r * math.copysign(abs(c) ** (2 / n), c), r + r * math.copysign(abs(s) ** (2 / n), s)))
    return "M" + "L".join(f"{f(x)},{f(y)}" for x, y in p) + "Z"


def svg(w, h, body):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {f(w)} {f(h)}" width="{f(w)}" height="{f(h)}">'
            f"<title>Hokuto</title>{body}</svg>\n")


def icon(small=False):
    """The icon's elements, 1024 units square. The small icon, for 32 pixels and less, has half as many lanes,
    longer notes and no beat lines. Its notes sit a little further apart vertically. That keeps them from running
    together."""
    lanes = 16
    L = 1024 / lanes
    body = ('<defs><linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">'
            '<stop offset="0" stop-color="#2b2f3a"/><stop offset="1" stop-color="#191b22"/></linearGradient>'
            '<linearGradient id="note" x1="0" y1="0" x2="0" y2="1">'
            '<stop offset="0" stop-color="#ffd977"/><stop offset="1" stop-color="#ffa63f"/></linearGradient>'
            f'<clipPath id="clip"><path d="{squircle()}"/></clipPath></defs>'
            '<g clip-path="url(#clip)"><rect width="1024" height="1024" fill="url(#bg)"/>')
    if small:
        for i in range(0, 8, 2):
            body += f'<rect y="{f(i*128)}" width="1024" height="128" fill="#000" fill-opacity=".18"/>'
        notes = fit(sky(10), (160, 170, 704, 684), stretch=1.25)
        nh, nl, line, outline, opacity = 104, 200, 24, 0, ".4"
    else:
        black = {1, 3, 6, 8, 10}
        for i in range(lanes):
            if (lanes - 1 - i) % 12 in black:
                body += f'<rect y="{f(i*L)}" width="1024" height="{f(L)}" fill="#000" fill-opacity=".22"/>'
            body += f'<rect y="{f(i*L - 1.5)}" width="1024" height="3" fill="#fff" fill-opacity=".05"/>'
        for i in range(1, 8):
            opacity = ".09" if i % 4 == 0 else ".04"
            body += f'<rect x="{f(i*128 - 1.5)}" width="3" height="1024" fill="#fff" fill-opacity="{opacity}"/>'
        stars = fit(sky(10), (170, 3 * L, 684, 10 * L))
        notes = {n: (x, (round(y / L - 0.5) + 0.5) * L) for n, (x, y) in stars.items()}
        nh, nl, line, outline, opacity = L * 0.72, L * 1.75, 10, 6, ".5"
    path = "".join(f"M{f(notes[a][0])},{f(notes[a][1])}L{f(notes[b][0])},{f(notes[b][1])}" for a, b in LINES)
    body += (f'<path d="{path}" fill="none" stroke="#ffc35c" stroke-opacity="{opacity}" stroke-width="{f(line)}" '
             f'stroke-linecap="round" stroke-linejoin="round"/>')
    stroke = f' stroke="#191b22" stroke-width="{f(outline)}"' if outline else ""
    for x, y in notes.values():
        body += (f'<rect x="{f(x - nl/2)}" y="{f(y - nh/2)}" width="{f(nl)}" height="{f(nh)}" rx="{f(nh/2)}" '
                 f'fill="url(#note)"{stroke}/>')
    return body + "</g>"


def macos(art):
    """Places icon elements on Apple's grid for macOS app icons: an 824-unit body, centred on the 1024-unit canvas,
    with a drop shadow."""
    k = 820 / 1024
    return ('<defs><filter id="shadow" x="-10%" y="-10%" width="120%" height="130%">'
            '<feDropShadow dx="0" dy="10" stdDeviation="12" flood-color="#000" flood-opacity=".3"/></filter></defs>'
            f'<path transform="translate(102 102) scale({k})" d="{squircle()}" filter="url(#shadow)"/>'
            f'<svg x="100" y="100" width="824" height="824" viewBox="0 0 1024 1024">{art}</svg>')


def wordmark(color, w=13.0, gap=15.0):
    """hokuto in rounded geometric lowercase. Returns (elements, width, height). The ascender is 96 units above the
    baseline, and the top-left corner is at 0,0."""
    xh, asc = 60.0, 96.0
    parts = []
    x = 0.0

    def Y(y):
        return asc - y

    def place(width, d):
        nonlocal x
        parts.append(f'<path transform="translate({f(x)} 0)" d="{d}"/>')
        x += width

    def stem(sx):
        return f"M{f(sx)},{f(Y(asc - w/2))}V{f(Y(w/2))}"

    def h():
        W = 56
        rr = (W - w) / 2
        cy = xh - w / 2 - rr
        place(W, stem(w / 2) + f"M{f(w/2)},{f(Y(cy))}A{f(rr)},{f(rr)} 0 0 1 {f(W-w/2)},{f(Y(cy))}V{f(Y(w/2))}")

    def o():
        r = (xh - w) / 2 + 1
        place(2 * r + w, f"M{f(w/2)},{f(Y(xh/2))}a{f(r)},{f(r)} 0 1 0 {f(2*r)},0a{f(r)},{f(r)} 0 1 0 {f(-2*r)},0Z")

    def k():
        W = 50
        tx, ty = W - w / 2, Y(xh - w / 2)
        jx, jy = w / 2 + 1, Y(xh * 0.27)
        mx, my = jx + (tx - jx) * 0.42, jy + (ty - jy) * 0.42
        place(W, stem(w / 2) + f"M{f(tx)},{f(ty)}L{f(jx)},{f(jy)}M{f(mx)},{f(my)}L{f(W - w/2)},{f(Y(w/2))}")

    def u():
        W = 56
        rr = (W - w) / 2
        cy = w / 2 + rr
        place(W, f"M{f(w/2)},{f(Y(xh - w/2))}V{f(Y(cy))}A{f(rr)},{f(rr)} 0 0 0 {f(W-w/2)},{f(Y(cy))}"
                 f"M{f(W-w/2)},{f(Y(xh - w/2))}V{f(Y(w/2))}")

    def t():
        W = 36
        s = w / 2 + 6
        rt = 12
        place(W, f"M{f(s)},{f(Y(asc*0.80))}V{f(Y(rt + w/2))}A{f(rt)},{f(rt)} 0 0 0 {f(s + rt)},{f(Y(w/2))}"
                 f"H{f(W - w/2)}M{f(w/2 - 2)},{f(Y(xh - w/2))}H{f(W - w/2)}")

    order = [h, o, k, u, t, o]
    for i, letter in enumerate(order):
        if i:
            x += gap - (3 if order[i] is o or order[i - 1] is o else 0)
        letter()
    body = (f'<g fill="none" stroke="{color}" stroke-width="{f(w)}" stroke-linecap="round" '
            f'stroke-linejoin="round">' + "".join(parts) + "</g>")
    return body, x, asc


def logo(color):
    """The icon with the wordmark to its right."""
    word, ww, wh = wordmark(color)
    k = 4.4
    gap = 250
    return svg(1024 + gap + ww * k, 1024,
               f'<svg width="1024" height="1024" viewBox="0 0 1024 1024">{icon()}</svg>'
               f'<g transform="translate({f(1024 + gap)} {f((1024 - wh * k) / 2)}) scale({f(k)})">{word}</g>')


# --- Rendering (macOS) ------------------------------------------------------------------------------

def png_chunks(data):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit("make.py: Quick Look didn't write a PNG file")
    pos = 8
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        yield kind, data[pos:pos + 12 + n]
        pos += 12 + n


def decode_png(data):
    """Returns (width, height, RGBA bytes) of an 8-bit RGB or RGBA PNG file without interlacing."""
    idat = b""
    for kind, chunk in png_chunks(data):
        if kind == b"IHDR":
            w, h, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", chunk[8:21])
        elif kind == b"IDAT":
            idat += chunk[8:-4]
    if depth != 8 or colour not in (2, 6) or interlace:
        raise SystemExit("make.py: Quick Look wrote a PNG format that this script doesn't read")
    bpp = 4 if colour == 6 else 3
    raw = zlib.decompress(idat)
    stride = w * bpp
    out = bytearray()
    prev = bytearray(stride)
    i = 0
    for _ in range(h):
        kind = raw[i]
        line = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if kind == 1:
                line[x] = (line[x] + a) & 255
            elif kind == 2:
                line[x] = (line[x] + b) & 255
            elif kind == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        if bpp == 4:
            out += line
        else:
            for x in range(w):
                out += line[x * 3:x * 3 + 3] + b"\xff"
    return w, h, bytes(out)


def dib(size, rgba):
    """A 32-bit bitmap for an ICO entry: the colours bottom-up, then the transparency mask."""
    colours = bytearray()
    mask = bytearray()
    mask_stride = (size + 31) // 32 * 4
    for y in reversed(range(size)):
        bits = bytearray(mask_stride)
        for x in range(size):
            r, g, b, a = rgba[(y * size + x) * 4:(y * size + x) * 4 + 4]
            colours += bytes((b, g, r, a))
            if a == 0:
                bits[x // 8] |= 0x80 >> (x % 8)
        mask += bits
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, len(colours) + len(mask), 0, 0, 0, 0)
    return header + bytes(colours) + bytes(mask)


def ico(entries):
    """An ICO file from (size, PNG data, RGBA bytes) entries. The 256-pixel entry stays PNG. The others become
    bitmaps. Every reader of ICO files supports those."""
    blobs = [png if size >= 256 else dib(size, rgba) for size, png, rgba in entries]
    out = struct.pack("<HHH", 0, 1, len(entries))
    offset = 6 + 16 * len(entries)
    for (size, _, _), blob in zip(entries, blobs):
        out += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    return out + b"".join(blobs)


def encode_png(size, rgba):
    """An 8-bit RGBA PNG file, without filters."""
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    stride = size * 4
    raw = b"".join(b"\0" + rgba[y * stride:(y + 1) * stride] for y in range(size))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"sRGB", b"\0") + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def plain_icns(data):
    """The ICNS file without the metadata that iconutil adds: the info entry, a property list that names the icon,
    and the eXIf chunk of each PNG entry. The icon's files hold nothing but pixels and colours."""
    if data[:4] != b"icns":
        raise SystemExit("make.py: iconutil didn't write an ICNS file")
    entries = b""
    pos = 8
    while pos < len(data):
        kind, n = struct.unpack(">4sI", data[pos:pos + 8])
        body = data[pos + 8:pos + n]
        pos += n
        if kind == b"info":
            continue
        if body[:8] == b"\x89PNG\r\n\x1a\n":
            body = body[:8] + b"".join(chunk for chunk_kind, chunk in png_chunks(body) if chunk_kind != b"eXIf")
        entries += struct.pack(">4sI", kind, 8 + len(body)) + body
    return b"icns" + struct.pack(">I", 8 + len(entries)) + entries


class Renderer:
    """Draws SVG bodies with Quick Look. Quick Look puts every image on white. Each body is therefore drawn twice,
    on black and on white. The difference between the two gives each pixel's alpha."""

    def __init__(self, tmp):
        self.tmp = tmp
        self.count = 0
        self.done = {}

    def plane(self, body, size, background):
        # Each render gets a new file name. Quick Look could otherwise return an earlier thumbnail.
        self.count += 1
        name = os.path.join(self.tmp, f"r{self.count}.svg")
        with open(name, "w", encoding="utf-8") as out:
            out.write(svg(1024, 1024, f'<rect width="1024" height="1024" fill="{background}"/>' + body))
        subprocess.run(["qlmanage", "-t", "-s", str(size), "-o", self.tmp, name],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with open(name + ".png", "rb") as png:
            w, h, rgba = decode_png(png.read())
        if (w, h) != (size, size):
            raise SystemExit(f"make.py: Quick Look drew {w} by {h} pixels instead of {size}")
        return rgba

    def __call__(self, body, size):
        """Returns (PNG data, RGBA bytes) of a 1024-unit SVG body drawn at size pixels."""
        if (body, size) not in self.done:
            black = self.plane(body, size, "#000")
            white = self.plane(body, size, "#fff")
            rgba = bytearray(len(black))
            for i in range(0, len(black), 4):
                spread = (white[i] - black[i]) + (white[i + 1] - black[i + 1]) + (white[i + 2] - black[i + 2])
                a = min(255, max(0, round(255 - spread / 3)))
                if a:
                    for c in range(3):
                        rgba[i + c] = min(255, round(black[i + c] * 255 / a))
                rgba[i + 3] = a
            if rgba[3] != 0:
                raise SystemExit("make.py: the icon's corners came out opaque")
            self.done[(body, size)] = (encode_png(size, bytes(rgba)), bytes(rgba))
        return self.done[(body, size)]


def render():
    if sys.platform != "darwin" or not shutil.which("qlmanage") or not shutil.which("iconutil"):
        raise SystemExit("make.py: --render requires macOS, with qlmanage and iconutil")
    tmp = tempfile.mkdtemp(prefix="hokuto-icon-")
    try:
        draw = Renderer(tmp)

        def art(size):
            return draw(icon(small=size <= SMALL), size)

        os.makedirs(os.path.join(HERE, "png"), exist_ok=True)
        for size in PNG_SIZES:
            with open(os.path.join(HERE, "png", f"hokuto-{size}.png"), "wb") as out:
                out.write(art(size)[0])
        with open(os.path.join(HERE, "hokuto.ico"), "wb") as out:
            out.write(ico([(size, *art(size)) for size in ICO_SIZES]))
        iconset = os.path.join(tmp, "hokuto.iconset")
        os.makedirs(iconset)
        for points in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                # A 16-point entry shows small even at twice the pixels. It takes the small icon.
                body = macos(icon(small=points <= SMALL))
                name = f"icon_{points}x{points}{'@2x' if scale == 2 else ''}.png"
                with open(os.path.join(iconset, name), "wb") as out:
                    out.write(draw(body, points * scale)[0])
        built = os.path.join(tmp, "hokuto.icns")
        subprocess.run(["iconutil", "-c", "icns", "-o", built, iconset], check=True)
        with open(built, "rb") as source, open(os.path.join(HERE, "hokuto.icns"), "wb") as out:
            out.write(plain_icns(source.read()))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    with open(os.path.join(HERE, "icon.svg"), "w", encoding="utf-8") as out:
        out.write(svg(1024, 1024, icon()))
    for mode, color in (("light", INK_LIGHT), ("dark", INK_DARK)):
        with open(os.path.join(HERE, f"logo-{mode}.svg"), "w", encoding="utf-8") as out:
            out.write(logo(color))
    if "--render" in sys.argv[1:]:
        render()


if __name__ == "__main__":
    main()
