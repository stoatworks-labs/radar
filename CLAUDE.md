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
- The fleet gate's expectation, drafted from what the plugins declare:
  `./build/ratest --expect` (committed as `docs/arena-expect.json`)

## Verify
- Everything: `tools/verify.sh` (~3 min: reserved words, no unbounded trig in the
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
- One raster only: add `--size WxH`. The software renderer:
  `RATEST_RENDERER=software ./build/ratest --arc --size 320x180`.
- **The checks can fail**: `--negative` (11 wrong models), `tools/mutate.sh`
  (one character of GLSL and of Sweep.h).
- What CI runs: `--offline` and `tools/glslc.sh`.
- No dead controls: `python3 tools/sweep.py` (43 parameters over both plugins).
- Cost: `--bench` (720p/1080p/4K, both plugins).

## Notes
- **The picture lives in a fixed polar grid**, 2048 bearings × 1024 ranges, never
  the host's raster: a resize cannot touch it.
- **A texel is its value at its column's last paint.** Decay is analytic: the
  CPU times every crossing in double and hands the composite a per-column
  fade and the paint a per-column carry. The GPU touches only the wedge.
- **Host indices are not ParamIds.** Each plugin declares its own dense list
  (`HostOrder()`), About block LAST in both, so a future "User guide" button
  moves nothing, and neither plugin declares a control it ignores.
- Range is normalised in the shaders (1 = the scope's edge); km, µs and degrees
  live in `Controls.cpp`. Every host parameter is 0..1 except Contacts and Seed
  (real integers).
- **No sin/cos/atan in the GLSL**: `sine`, `cosine`, `bearingOf` (verify.sh
  greps). GLSL 4.10 bounds none of them and the software renderer's sin was 3%
  off.
- **No `1 - exp( -p )` for small p** in float: the video uses the series.
- GLSL reserved words must not be identifiers (including `packed`); verify.sh
  greps for the 4.10 list.
- Randomness is PCG integer hashing on both sides, never `fract(sin(...))`.
- `radar_core` is an OBJECT library: the registrations are file-scope
  constructors nothing references.
- Local repo only: no GitHub remote, no tag, not registered on the website.

## Browser demo
- `demo/` is the page at https://radar-demo.stoatworks-labs.com (Cloudflare
  Worker `radar-demo`, a ROUTE on a proxied AAAA 100:: record -- the zone's
  custom domains are full; deleting the record takes the page dark with a green
  deploy). `deploy.yml` redeploys it on a push to main; by hand:
  `cf-run npx wrangler deploy`.
- `demo/shaders.js` is GENERATED: after changing `source/Shaders.cpp` run
  `python3 demo/tools/check_shaders.py --write`; verify.sh fails on drift.
- The CPU half in `demo/plugin.js` (Controls, Sweep, the per-column timing,
  World) is a hand port nobody checks but a reader: change it with the C++.
- `demo/vendor/` is the shared kit: never edit it, re-vendor with
  `stoatworks-backend/resolume-demo/sync.sh`.

## Not done yet
- Never loaded into Resolume (oxbow selftest only). Never built on Windows. No
  OpenFX port.
- `StoatworksAbout.h` and `ATTRIBUTIONS.md` are provisional hand copies.

## Diagnostics

`source/Diag.{h,cpp}` is a log file only, with no crash handler (this runs
inside Resolume). It records which shader failed to compile and the GL
vendor/renderer.

    ~/Library/Logs/radar/radar.YYYY-MM-DD.log
