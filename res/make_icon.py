"""Turns res/app.svg (the app icon by nik-txt) into res/app.ico (white: the exe, Start menu and installer, and
dark themes) and res/app_light.ico (black: the request window on light themes). Needs Python 3 and Pillow.

Only the simple SVG subset the icon uses is understood: <path d="..."> with M L H V C S Z commands
(absolute and relative), filled even-odd. Each size is drawn 8x larger and scaled down for smooth edges.
"""
import io
import os
import re
import struct
from PIL import Image, ImageChops, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
SIZES = [16, 20, 24, 32, 40, 48, 64, 256]
VERSIONS = {"app.ico": (255, 255, 255), "app_light.ico": (0, 0, 0)}
SUPER = 8


def subpaths(d):
    """Yields each subpath of a path as a list of points (curves flattened)."""
    tokens = re.findall(r"[MmLlHhVvCcSsZz]|-?(?:\d+\.?\d*|\.\d+)(?:e-?\d+)?", d)
    i, cmd = 0, None
    x = y = sx = sy = 0.0
    last_ctrl = None
    pts = []

    def num():
        nonlocal i
        v = float(tokens[i])
        i += 1
        return v

    def bezier(p0, p1, p2, p3):
        for k in range(1, 17):
            t = k / 16
            a, b, c, e = (1 - t) ** 3, 3 * (1 - t) ** 2 * t, 3 * (1 - t) * t * t, t ** 3
            pts.append((a * p0[0] + b * p1[0] + c * p2[0] + e * p3[0], a * p0[1] + b * p1[1] + c * p2[1] + e * p3[1]))

    while i < len(tokens):
        if re.match(r"[A-Za-z]", tokens[i]):
            cmd = tokens[i]
            i += 1
        rel = cmd.islower()
        c = cmd.upper()
        ctrl = None
        if c == "M":
            if len(pts) > 1:
                yield pts
            x, y = (x + num(), y + num()) if rel else (num(), num())
            sx, sy = x, y
            pts = [(x, y)]
            cmd = "l" if rel else "L"  # extra pairs after M are lines
        elif c == "L":
            x, y = (x + num(), y + num()) if rel else (num(), num())
            pts.append((x, y))
        elif c == "H":
            x = x + num() if rel else num()
            pts.append((x, y))
        elif c == "V":
            y = y + num() if rel else num()
            pts.append((x, y))
        elif c in "CS":
            if c == "C":
                x1, y1 = num(), num()
                if rel:
                    x1, y1 = x + x1, y + y1
            else:
                x1, y1 = (2 * x - last_ctrl[0], 2 * y - last_ctrl[1]) if last_ctrl else (x, y)
            x2, y2, ex, ey = num(), num(), num(), num()
            if rel:
                x2, y2, ex, ey = x + x2, y + y2, x + ex, y + ey
            bezier((x, y), (x1, y1), (x2, y2), (ex, ey))
            x, y = ex, ey
            ctrl = (x2, y2)
        elif c == "Z":
            x, y = sx, sy
            if len(pts) > 1:
                yield pts
            pts = [(x, y)]
        last_ctrl = ctrl
    if len(pts) > 1:
        yield pts


def render(paths, size, box, color):
    big = size * SUPER
    x0, y0, side = box
    scale = big / side
    mask = Image.new("1", (big, big), 0)
    for d in paths:
        for sp in subpaths(d):
            layer = Image.new("1", (big, big), 0)
            ImageDraw.Draw(layer).polygon([((px - x0) * scale, (py - y0) * scale) for px, py in sp], fill=1)
            mask = ImageChops.logical_xor(mask, layer)  # even-odd: overlapping subpaths cut holes
    alpha = mask.convert("L").resize((size, size), Image.LANCZOS)
    img = Image.new("RGBA", (size, size), color + (0,))
    img.putalpha(alpha)
    return img


def main():
    svg = open(os.path.join(HERE, "app.svg"), encoding="utf-8").read()
    paths = [re.sub(r"\s+", " ", d) for d in re.findall(r'<path[^>]*\sd="([^"]+)"', svg)]
    # Fit the drawing's bounds into a square with a small margin.
    xs, ys = [], []
    for d in paths:
        for sp in subpaths(d):
            xs += [p[0] for p in sp]
            ys += [p[1] for p in sp]
    w, h = max(xs) - min(xs), max(ys) - min(ys)
    side = max(w, h) * 1.04
    box = ((min(xs) + max(xs)) / 2 - side / 2, (min(ys) + max(ys)) / 2 - side / 2, side)

    for name, color in VERSIONS.items():
        pngs = []
        for s in SIZES:
            buf = io.BytesIO()
            render(paths, s, box, color).save(buf, "PNG")
            pngs.append(buf.getvalue())
        out = io.BytesIO()
        out.write(struct.pack("<HHH", 0, 1, len(SIZES)))
        offset = 6 + 16 * len(SIZES)
        for s, p in zip(SIZES, pngs):
            d = 0 if s >= 256 else s
            out.write(struct.pack("<BBBBHHII", d, d, 0, 0, 1, 32, len(p), offset))
            offset += len(p)
        for p in pngs:
            out.write(p)
        open(os.path.join(HERE, name), "wb").write(out.getvalue())
        print("wrote", name + ":", ", ".join(map(str, SIZES)))


if __name__ == "__main__":
    main()
