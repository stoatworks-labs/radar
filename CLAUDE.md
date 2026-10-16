# radar

A plan-position radar scope for Resolume Arena/Avenue, as two FFGL plugins from
one core: `SW Radar` (`RA01`, source: a synthetic sea) and `SW Radar Over`
(`RA02`, effect: the clip is the sea). C++/GLSL, CMake MODULE → two universal
`.bundle`s (macOS) + Windows `.dll`s. MIT. Bundle ids `com.stoatworks.ffgl.radar`
and `com.stoatworks.ffgl.radar.over`.

Read `AGENTS.md` before touching the sweep (`Sweep.h`), the per-column timing in
`Radar.cpp`, the beam pattern or the parameter tables.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build` (never from `~/Projects`)
- Render the source: `./build/ratest --out /tmp/scope.png --frames 600`
- Render the Over effect on the harness's card: `./build/ratest --over --out /tmp/o.png`
- A frame of a real clip through it: `ffmpeg -i clip.mov -frames:v 1 -s 1280x720 -f rawvideo -pix_fmt rgba /tmp/c.rgba && ./build/ratest --over --clip /tmp/c.rgba --out /tmp/o.png`
- List parameters: `./build/ratest --list` (`--over` for the effect's)
- Set anything by name: `./build/ratest --set "Phosphor=1" --set "Range=0.8"`
- Sprites (radar#5): `./build/ratest --set "Sprite Count=10" --set "Sprite File=$PWD/tools/fixtures/alien.png"`
  (a file parameter takes its path as text; empty is the built-in blip). Since v0.2.0:
  `--set "Path=1"` (Transit), `--set "Start With=3"`, `--set "Orientation=2"` (Head In),
  `--set "Sprite Colour=1"` (Picture). `docs/transit.png` is the first with Path 1, Sprite Size
  0.6045 (0.14 radii) and `--frames 940`.
- The source as video: `./build/ratest --pipe --frames 1800 --size 1280x720 --script cues.txt | ffmpeg -f rawvideo -pix_fmt rgba -s 1280x720 -r 60 -i - out.mp4`
- A clip through the effect: `ffmpeg -i in.mov -f rawvideo -pix_fmt rgba -s WxH - | ./build/ratest --over --pipe --size WxH | ffmpeg …`
  A cue line is `frame  Parameter Name  value` (`#` starts a comment), in the same
  units as `--set`. Held before the first key and after the last; a standard
  control is linear between keys, and an option, boolean, event or integer
  STEPS (holds each key until the next key's frame), so a press is three keys
  (0, 1, 0). Frame *n* is clocked at n / `--fps` (60). An unknown name exits 2
  before any frame; a partial frame at EOF ends the stream with exit 0; a reader
  that hangs up ends `--pipe`/`--film` with exit 1 (SIGPIPE is ignored), never a
  silent 141. The source's `--pipe` with no `--frames` runs until the reader
  hangs up, so it only ever ends with exit 1.
- A draft of the fleet gate's expectation from what the plugins declare:
  `./build/ratest --expect` (a draft only: its `note` fields are not the gate's).
  `docs/arena-expect.json` is a COPY of the gate's real expectation,
  `plugin-bench/arena/expect/radar.json` (seeded from the running Arena).

## Verify
- Everything: `tools/verify.sh` (~7 min: reserved words, no unbounded trig in the
  GLSL, glslc, fresh universal build, both bundles through
  lipo/plist/codesign/oxbow probe+selftest, every check at 1280x720 and 320x180,
  the same checks on Apple's software renderer at 320x180, the offline set, the
  `--pipe` contract, the negative controls, the mutants, the sweep, the bench)
- **The radar**: `--arc` (beamwidth), `--pulse` (c tau / 2), `--r4` (R^-4 and
  STC), `--over-check` (the clip's polar mapping).
- **The sweep and the phosphor**: `--sweep` (every bin once a rotation, any
  frame rate), `--persist` (the two-term decay, the repaint), `--resize`,
  `--clock` (Resolume's 499 million ms).
- **The plugin**: `--prime`, `--state`; no GL: `--sweep-law`, `--cues`, `--names`.
- **The sprites** (radar#5): `--sprite-file` (alpha / brightest channel, the blip
  for a bad, missing or truncated file, a non-ASCII path), `--sprite-threshold`,
  `--sprites` (count, size, orientation, growth off the painted picture),
  `--sprite-speed` (off the phosphor at 60 and 24 fps), `--sprite-beam` (dark until
  the sweep, then the phosphor's decay), `--sprite-repeat` (same frame twice),
  `--sprite-colour` (and Picture's); no GL: `--sprite-utf16` (the Windows path), `--sprite-motion`.
  Since v0.2.0: `--sprite-transit` (Transit's track read off the phosphor column by column, the
  weave, Grow on Transit), `--sprite-arrivals` (Start With: the painted count rotation by
  rotation against the schedule, all in before the first leaves, the same twice); no GL:
  `--sprite-path` (Transit's law, the entry distribution, the weave, the latecomers' law).
  Fixtures in `tools/fixtures/` (`make_fixtures.py` draws them).
- One raster only: add `--size WxH`. The software renderer:
  `RATEST_RENDERER=software ./build/ratest --arc --size 320x180`.
- **The checks can fail**: `--negative` (28 wrong models), `tools/mutate.sh`
  (one character of GLSL, of Sweep.h, Swarm, Radar.cpp and SpriteImage: 10 mutants).
- What CI runs: `--offline` and `tools/glslc.sh`.
- No dead controls: `python3 tools/sweep.py` (71 parameters over both plugins; the
  sprite controls are swept with ten sprites on, Orientation, Sprite File and Sprite Colour
  with `tools/fixtures/alien.png`; Path and Start With from the first frame).
- Cost: `--bench` (720p/1080p/4K, both plugins).

## Notes
- **The picture lives in a fixed polar grid**, 2048 bearings × 1024 ranges, never
  the host's raster: a resize cannot touch it.
- **A texel is its value at its column's last paint.** Decay is analytic: the
  CPU times every crossing in double and hands the composite a per-column
  fade and the paint a per-column carry. The GPU touches only the wedge.
- **Four components per texel** since the sprites: (flash, afterglow) of
  everything, then of the sprites' share (RGBA32F phosphor and scratch; the
  reflect band is RG32F: surface, sprites). The share only recolours the sprites
  (Hue, Saturation); at Hue 0 / Saturation 0.5 the composite skips it.
- **Sprites are targets, not overlays**: the reflect pass samples their mask
  like the coast, the paint convolves it with the beam, the sweep paints it.
  Their motion is `Swarm.cpp` (closed-form in host time); the file is
  `SpriteImage.cpp` (stb_image, `_wfopen_s` + `Utf8ToUtf16` on Windows), loaded
  lazily on the render thread. Sprite Count defaults to 0: off.
- **Since v0.2.0 (unreleased): Path, Start With, Sprite Colour** appended after Grow On
  Approach (only the About buttons moved), and Head In appended to Orientation. Every default
  is v0.2.0's picture bit for bit (AGENTS.md says how that was shown). Path is latched per
  generation; Start With's latecomers wait a distance counted at Sprite Speed; the picture's
  colour is a mask-weighted mean computed at load. No shader changed.
- **Host indices are not ParamIds.** Each plugin declares its own dense list
  (`HostOrder()`), About block LAST in both, so the "User guide" button moved no
  control (it sits second inside the About block), and neither plugin declares a
  control it ignores.
- Range is normalised in the shaders (1 = the scope's edge); km, µs and degrees
  live in `Controls.cpp`. Every host parameter is 0..1 except Contacts, Seed and
  Sprite Count (real integers) and Sprite File (a file parameter: a path).
- **No sin/cos/atan in the GLSL**: `sine`, `cosine`, `bearingOf` (verify.sh
  greps). GLSL 4.10 bounds none of them and the software renderer's sin was 3%
  off.
- **No `1 - exp( -p )` for small p** in float: the video uses the series.
- GLSL reserved words must not be identifiers (including `packed`); verify.sh
  greps for the 4.10 list.
- Randomness is PCG integer hashing on both sides, never `fract(sin(...))`.
- `radar_core` is an OBJECT library: the registrations are file-scope
  constructors nothing references.
- Public at `github.com/stoatworks-labs/radar`, released v0.1.0 on 2026-09-25 and v0.2.0
  (the sprites, radar#5) on 2026-10-15, registered on the website.

## Browser demo
- `demo/` is the page at https://radar-demo.stoatworks-labs.com (Cloudflare
  Worker `radar-demo`, a ROUTE on a proxied AAAA 100:: record -- the zone's
  custom domains are full; deleting the record takes the page dark with a green
  deploy). `deploy.yml` redeploys it on a push to main; by hand:
  `cf-run npx wrangler deploy`.
- `demo/shaders.js` is GENERATED: after changing `source/Shaders.cpp` run
  `python3 demo/tools/check_shaders.py --write`; verify.sh fails on drift.
- The CPU half in `demo/plugin.js` (Controls, Sweep, the per-column timing,
  World, Swarm, the sprite mask) is a hand port nobody checks but a reader:
  change it with the C++. `demo/sprites/` holds copies of the two fixtures.
- `demo/vendor/` is the shared kit: never edit it, re-vendor with
  `stoatworks-backend/resolume-demo/sync.sh`.

## Not done yet
- Never loaded into Resolume on macOS (oxbow selftest only). On Windows: the fleet's
  Arena gate (see AGENTS.md), at v0.2.0, sprites included. No OpenFX port.
- **Path, Start With, Head In and Sprite Colour (since v0.2.0, built on `claude/transit`,
  not released) have never been in Resolume**, on either platform: harness and browser demo only.
  They need the version bump and release when Allan wants them, and the README's and guide's
  "not released yet" lines need changing then.
- **plugin-bench's `arena/expect/radar.json` needs the three new rows** (Path, Start With,
  Sprite Colour) and Orientation's third element, and `docs/arena-expect.json` (its copy) with
  it; `ratest --expect` drafts them with notes. The gate can't see Path or Start With change on a
  running layer (both act only on sprites that start): expect inconclusive or a note-driven
  sequence, not "live".
- Sprites: one picture for all of them; two paths (no sector, no other heading); Picture colour is
  one mean colour, not per-pixel. Still open from radar#5: several files (Steve: no), a flash or
  pass-through at the centre (Transit is his pass-through), more than 32.
- `StoatworksAbout.h`, `ATTRIBUTIONS.md`, `.github/FUNDING.yml` and `.github/ISSUE_TEMPLATE/`
  are GENERATED by the backend's sync scripts; the user guide is `docs/USER-GUIDE.md` (PDF
  generated).

## Diagnostics

`source/Diag.{h,cpp}` is a log file only, with no crash handler (this runs
inside Resolume). It records which shader failed to compile and the GL
vendor/renderer.

    ~/Library/Logs/radar/radar.YYYY-MM-DD.log
