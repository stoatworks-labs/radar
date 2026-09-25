#!/usr/bin/env python3
"""Render every parameter at both ends of its range, in both plugins, and fail
if any made no difference.

**This is the only thing in the repo that catches a dead control.** A GLSL
uniform whose name does not match the C++ is silently ignored --
`glGetUniformLocation` returns -1 and `glUniform` on -1 is a documented no-op
-- so a slider can be stone dead while everything compiles, links, loads and
renders.

Each control is swept where it can act: the audio controls with a beat fed in,
Threshold and Mix through the Over effect on the harness's harbour card. An
option parameter reads back 0..1 whatever its count (the fleet's trap), so
options are set here by element index. A control whose ends differ by less than
`--floor` (mean 8-bit difference per channel) is reported as barely alive:
vectrix's lesson, a control can be alive, correct and still useless.

Every render is 320x180 (CI's raster) and 160 frames at 60 fps: one rotation of
the default 24 rpm antenna and a little more, so every bearing has been painted.

Usage::

    tools/sweep.py [--build BUILD_DIR] [--verbose] [--jobs N]
"""

import argparse
import concurrent.futures
import pathlib
import re
import subprocess
import sys
import tempfile
import zlib

REPO = pathlib.Path(__file__).resolve().parent.parent

# name -> (low setting, high setting, context settings, extra flags)
SWEEP = {
    "RPM": ("0.4", "0.9", [], []),
    "Beamwidth": ("0", "1", [], []),
    "Sidelobes": ("0", "1", ["Gain=0.7", "Land=0", "Contacts=16"], []),
    "Direction": ("0", "1", ["Persistence=0.3"], []),
    "Pulse Length": ("0", "1", [], []),
    "Range": ("0.3", "0.8", [], []),
    "Gain": ("0.2", "0.8", [], []),
    "STC": ("0", "1", [], []),
    "Clutter": ("0", "1", [], []),
    "Noise": ("0", "1", [], []),
    "Contacts": ("0", "16", [], []),
    "Rain": ("0", "1", [], []),
    "Land": ("0", "1", [], []),
    "Seed": ("1", "2", [], []),
    "Threshold": ("0", "0.8", [], []),
    "Audio Strobe": ("0", "1", [], ["beat"]),
    "Audio Contacts": ("0", "1", ["Beamwidth=0.8", "Pulse Length=0.8", "Land=0"], ["beat"]),
    "Persistence": ("0", "1", [], []),
    "Flash": ("0", "1", ["Gain=0.6", "Land=1"], []),
    "Phosphor": ("0", "2", [], []),
    "Rings": ("0", "1", [], []),
    "Bearing Marks": ("0", "1", ["Scope Size=0"], []),
    "Heading Line": ("0", "1", ["Scope Size=0"], []),
    "Scope Size": ("0", "1", [], []),
    "Mix": ("0", "1", [], []),
}

# Parameters with no pixel to sweep: the FFT buffer (its float is meaningless;
# Audio Strobe and Audio Contacts are its sweepable proof) and the About block
# (a text line and browser buttons).
SKIP = {"Audio", "About", "Project page", "Source on GitHub", "Support the work", "User guide"}


def read_png(path):
    """Enough of PNG for ratest's own writer: 8-bit RGBA, filter 0 rows."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, width, height, idat = 8, 0, 0, b""
    while pos < len(data):
        length = int.from_bytes(data[pos:pos + 4], "big")
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width = int.from_bytes(body[0:4], "big")
            height = int.from_bytes(body[4:8], "big")
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray()
    for y in range(height):
        start = y * (stride + 1)
        out += raw[start + 1:start + 1 + stride]
    return bytes(out)


def difference(a, b):
    if len(a) != len(b):
        return 255.0
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def render(ratest, out, settings, extra, effect):
    args = [str(ratest), "--out", str(out), "--size", "320x180", "--frames", "160"]
    if effect:
        args.append("--over")
    if "beat" in extra:
        args.append("--beat")
    for setting in settings:
        args += ["--set", setting]
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"ratest failed: {' '.join(args)}\n{result.stderr.strip()}")
    return read_png(out)


def parameters(ratest, effect):
    result = subprocess.run([str(ratest), "--list"] + (["--over"] if effect else []), capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"ratest --list failed: {result.stderr.strip()}")
    names = []
    for line in result.stdout.splitlines()[1:]:
        parts = re.split(r"\s{2,}", line.strip())
        if len(parts) >= 3 and parts[0].isdigit():
            names.append(parts[1].strip())
    return names


def sweep_one(ratest, scratch, effect, name):
    low, high, context, extra = SWEEP[name]
    tag = f"{'o' if effect else 's'}{abs(hash(name))}"
    a = scratch / f"{tag}-a.png"
    b = scratch / f"{tag}-b.png"
    before = render(ratest, a, context + [f"{name}={low}"], extra, effect)
    after = render(ratest, b, context + [f"{name}={high}"], extra, effect)
    return effect, name, difference(before, after)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=pathlib.Path, default=REPO / "build")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--floor", type=float, default=0.05,
                        help="mean 8-bit difference below which a control is 'barely alive'")
    args = parser.parse_args()

    build = args.build if args.build.is_absolute() else REPO / args.build
    ratest = build / "ratest"
    if not ratest.exists():
        print(f"{ratest} not found", file=sys.stderr)
        return 1

    jobs = []
    for effect in (False, True):
        declared = parameters(ratest, effect)
        unknown = [n for n in declared if n not in SWEEP and n not in SKIP]
        if unknown:
            # A new parameter with no sweep is a hole, not a pass.
            print(f"no sweep defined for: {', '.join(unknown)}", file=sys.stderr)
            return 1
        jobs += [(effect, n) for n in declared if n in SWEEP]
    unused = [n for n in SWEEP if not any(n == j[1] for j in jobs)]
    if unused:
        print(f"the sweep names parameters neither plugin has: {', '.join(unused)}", file=sys.stderr)
        return 1

    dead, weak = [], []
    with tempfile.TemporaryDirectory() as scratch:
        scratch = pathlib.Path(scratch)
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            results = list(pool.map(lambda j: sweep_one(ratest, scratch, j[0], j[1]), jobs))
    for effect, name, delta in results:
        who = "SW Radar Over" if effect else "SW Radar"
        if delta == 0.0:
            dead.append(f"{who}: {name}")
            print(f"  DEAD {who:14s} {name:16s} both ends identical")
        elif delta < args.floor:
            weak.append(f"{who}: {name}")
            print(f"  WEAK {who:14s} {name:16s} mean delta {delta:.4f}")
        elif args.verbose:
            print(f"  ok   {who:14s} {name:16s} mean delta {delta:.3f}")

    print(f"{len(results)} parameters swept over both plugins, {len(dead)} dead, {len(weak)} barely alive")
    if dead or weak:
        print("\nA parameter that changes nothing is usually a uniform name that does not match\n"
              "the C++, or a setting nothing reads. Both are silent everywhere else.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
