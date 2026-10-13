#!/usr/bin/env python3
"""Draw the sprite fixtures (radar#5) from geometry: nothing here is copied
from an emoji set or anyone's artwork.

    alien.png   96x96 RGBA: a green head, wide at the top, with two slanted
                eyes cut out (transparent), anti-aliased by 4x4 supersampling.
                The sprite with transparency: its ALPHA is the mask.
    alien.jpg   the same head on black, no alpha: the mask is its BRIGHTEST
                CHANNEL. Written by macOS `sips` from a flattened PNG.
    panda.png   96x96 RGBA: a round white head with two black ears and black
                eye patches -- the patches are opaque and dark, so the alpha
                mask is the whole silhouette, ears and all.
    broken.png  not an image: text with a .png name, for the bad-file check.

tools/sweep.py and `ratest --sprite-file` use them; the README's and the
guide's sprite pictures were rendered with them. alien.png and panda.png are
also written to demo/sprites/, the browser demo's two pictures.

    python3 tools/fixtures/make_fixtures.py
"""

import math
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = pathlib.Path(__file__).resolve().parent
N = 96
SS = 4


def png(path, width, height, rgba):
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        raw += bytes(rgba[y * width * 4:(y + 1) * width * 4])

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    data = b"\x89PNG\r\n\x1a\n"
    data += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    data += chunk(b"IEND", b"")
    path.write_bytes(data)


def render(shade):
    """shade(u, v) -> (r, g, b, a) in 0..1, u right and v DOWN, both -1..1."""
    out = bytearray(N * N * 4)
    for y in range(N):
        for x in range(N):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sy in range(SS):
                for sx in range(SS):
                    u = ((x + (sx + 0.5) / SS) / N) * 2.0 - 1.0
                    v = ((y + (sy + 0.5) / SS) / N) * 2.0 - 1.0
                    r, g, b, a = shade(u, v)
                    # premultiplied while averaging, straight when stored
                    acc[0] += r * a
                    acc[1] += g * a
                    acc[2] += b * a
                    acc[3] += a
            a = acc[3] / (SS * SS)
            rgb = [c / acc[3] if acc[3] > 0 else 0.0 for c in acc[:3]]
            o = (y * N + x) * 4
            out[o:o + 4] = bytes(int(round(max(0.0, min(1.0, c)) * 255)) for c in rgb + [a])
    return out


def alien(u, v):
    # A head: an ellipse, squeezed towards the chin (v > 0 is down).
    w = 0.78 * (1.0 - 0.45 * max(0.0, v + 0.1))
    head = (u / w) ** 2 + ((v + 0.05) / 0.9) ** 2 <= 1.0
    if not head:
        return (0, 0, 0, 0)
    # Two eyes, slanted outward and up, cut out.
    for side in (-1.0, 1.0):
        cu, cv = 0.32 * side, 0.02
        ang = 0.55 * side
        du, dv = u - cu, v - cv
        ru = du * math.cos(ang) - dv * math.sin(ang)
        rv = du * math.sin(ang) + dv * math.cos(ang)
        if (ru / 0.24) ** 2 + (rv / 0.12) ** 2 <= 1.0:
            return (0, 0, 0, 0)
    return (0.45, 0.95, 0.35, 1.0)


def panda(u, v):
    for side in (-1.0, 1.0):
        if (u - 0.58 * side) ** 2 + (v + 0.6) ** 2 <= 0.26 ** 2:
            return (0.05, 0.05, 0.05, 1.0)
    if u * u + (v * 1.08) ** 2 > 0.78 ** 2:
        return (0, 0, 0, 0)
    for side in (-1.0, 1.0):
        du, dv = u - 0.3 * side, v + 0.02
        ang = 0.6 * side
        ru = du * math.cos(ang) - dv * math.sin(ang)
        rv = du * math.sin(ang) + dv * math.cos(ang)
        if (ru / 0.17) ** 2 + (rv / 0.24) ** 2 <= 1.0:
            return (0.05, 0.05, 0.05, 1.0)
    if u * u / 0.12 ** 2 + (v - 0.28) ** 2 / 0.08 ** 2 <= 1.0:
        return (0.05, 0.05, 0.05, 1.0)
    return (0.97, 0.97, 0.95, 1.0)


def flatten(rgba):
    out = bytearray(rgba)
    for i in range(0, len(out), 4):
        a = out[i + 3] / 255.0
        for c in range(3):
            out[i + c] = int(round(out[i + c] * a))
        out[i + 3] = 255
    return out


def main():
    head = render(alien)
    bear = render(panda)
    demo = HERE.parent.parent / "demo" / "sprites"
    demo.mkdir(parents=True, exist_ok=True)
    for folder in (HERE, demo):
        png(folder / "alien.png", N, N, head)
        png(folder / "panda.png", N, N, bear)
    with tempfile.TemporaryDirectory() as scratch:
        flat = pathlib.Path(scratch) / "alien-flat.png"
        png(flat, N, N, flatten(head))
        result = subprocess.run(["sips", "-s", "format", "jpeg", "-s", "formatOptions", "90", str(flat),
                                 "--out", str(HERE / "alien.jpg")], capture_output=True, text=True)
        if result.returncode != 0:
            print(result.stderr, file=sys.stderr)
            return 1
    (HERE / "broken.png").write_text("This is not a PNG. ratest --sprite-file loads it and must fall back to the blip.\n")
    print("wrote alien.png, alien.jpg, panda.png, broken.png (and demo/sprites/)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
