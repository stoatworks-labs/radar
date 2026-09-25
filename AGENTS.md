# radar — for agents

The why behind the code. `CLAUDE.md` has the commands; this file has the
reasoning, the traps that were actually hit, and what is and is not known.
Built 2026-09-25 in one session, tranche five, from an idea Allan picked
himself. The spec is `~/Projects/resolume/specs/SPEC-radar.md`; the briefs are
`~/Projects/resolume/specs/BRIEF.md` and `BRIEF-ADDENDUM.md`.

## The one idea

**A plan-position indicator is a CRT whose trace runs out from the centre along
the antenna's bearing, and the antenna turns.** Each echo brightens a
long-persistence phosphor at its range and bearing. What the scope shows is
not the sea: it is **the sea convolved with the radar**. So nothing here is
drawn as a picture of a radar. A reflectivity field (a synthetic coast in the
source, the clip in the Over effect) goes through a radar model, and the
look falls out:

- a point target paints an **arc as wide as the beam**, so far targets are
  wide arcs and near ones short;
- it paints a **radial streak c tau / 2 long**, outward from its range;
- the **sweep leaves a trail**: what was painted a moment ago is bright, what
  was painted a rotation ago is about to be repainted;
- **clutter** crowds the centre and **STC** pushes it down; **noise** is speckle;
  a coast facing the radar is a **bright edge** and the land behind it falls
  into **shadow**;
- **moving contacts** leave the plot of their track in their fading paints.

## How a frame goes

Everything lives in a fixed polar grid, **2048 bearing bins x 1024 range
bins**, never the host's raster (`Controls.h`, `kBearings`/`kRanges`).

1. **The antenna** (`Radar.cpp`, `Sweep.h`): its position `b` in bins, double,
   unwrapped, advances by rate x dt, dt the host's real elapsed time
   (seconds or milliseconds, voted against the wall clock, rosette's code). A
   frame paints the bins whose centres it **crossed**: `(b0, b1]` going
   clockwise, `[b1, b0)` anticlockwise. Consecutive half-open intervals abut,
   so every bin is painted exactly once a rotation at any frame rate, with no
   rasteriser involved. That is the analytic wedge: the swept sector,
   rasterised onto the bearing grid by the one rule that tiles a rotation.
2. **Timing, on the CPU in double**: each crossed column's crossing time
   (clock minus (b1 - k)/(b1 - b0) x dt), and from it the **Carry** (the
   decay from the column's previous crossing to this one) and every column's
   **Fade** (the decay from its last crossing to now), both uploaded as
   2048 x 1 textures.
3. **Kernel** (on change): the beam pattern tabulated per bin offset, by the
   same GLSL function the point targets use.
4. **Map** (source, on change): the synthetic coast as a 1024^2 square over
   the scope, fBm in KM so Range zooms one map; red is what the surface
   returns, green how much land is there.
5. **Reflect**, for the bearing band the beam can reach from this wedge: the
   surface resampled in range x bearing and averaged over the pulse's range
   footprint `[r - L, r]`; in the source, times the radar shadow
   `exp( -land crossed / 1.5 km )`; in the Over, the clip's brightest channel
   over Threshold, times alpha.
6. **Paint**, the wedge's columns only, into a scratch buffer copied back over
   the phosphor's same columns: `S' = S x Carry + echo x (Flash, 1)`.
7. **Composite**: each screen pixel's range and bearing (north up, clockwise)
   read its four nearest texels, EACH faded by its own column, and
   interpolate by hand; the two components glow in their colours; rings,
   bearing marks and the heading line one pixel wide; `1 - exp( -light )`.

### The echo

    P = G [ r^(n-4) (surface (x) beam + clutter + rain) + R_t^(n-4) points + r^n noise ] + strobe
    video = 1 - exp( -P )

- **R^-4, normalised**: 1 at the scope's edge, so Gain means the same at any
  Range. **STC** is the exponent n (0..4): a gain rising as r^n. At n = 4 the
  radar equation is flat.
- **Point targets are analytic** in both directions: sigma x beam( their
  bearing - the bin's ) x the fraction of the range bin their pulse
  `[R_t, R_t + L]` covers. STC for a point is applied at the target's range,
  not along its streak (decision: the check can then be exact; the difference
  is (1 + L/R)^n along a streak, invisible).
- **The surface's beam convolution is normalised**: a wider beam blurs a
  coast, it does not brighten it. Physically a distributed target's return
  grows with the beam and the pulse; here both only blur (decision, for an
  operator: Beamwidth and Pulse Length should not also be gain knobs).
- **Clutter** is area-extensive (x r) and falls with range in km
  (0.6 + 4 x Clutter km); **rain** is volume-extensive (x r^2), drifting cells;
  both speckle pulse to pulse (exponential power, PCG). **Noise** enters
  before the STC gain, so STC pushes it down near the centre too.
- **The video's series**: see the traps.

### The beam

`beam( off )` is the TWO-WAY power pattern, 1 on the axis and exactly 1/2 at
+-Beamwidth/2. **Beamwidth is therefore the width of the arc a point target
paints**, the two-way half-power width; a one-way (antenna datasheet) beamwidth
is sqrt 2 wider for the Gaussian. Decision: an operator setting 2 degrees
should see 2-degree arcs. **Sidelobes** mixes the Gaussian with a uniform
aperture's sinc^4 scaled to the same half-power width. Both are 1/2 at the
half-power points and monotone through them, so any mix has the same width,
and `--arc` holds at Sidelobes 0 and 1.

### The phosphor

Two components per texel, (flash, afterglow). The flash's time constant is
the phosphor's (P7 35 ms, P19 25 ms, Green 60 ms), the afterglow's is
Persistence (0.1 - 30 s); Flash is the flash's strength against the
afterglow's 1. Real P7 flashes in microseconds; the flash here is stretched to
a few frames so a 60 fps picture can show it: the bright leading edge of the
sweep. Colours: P7 blue-white flash, yellow-green afterglow; P19 orange; Green
green.

A texel stores its value **as its column's last paint left it**; nothing
decays on the GPU between paints. The CPU knows when every column was
crossed, in double, and the composite multiplies by `exp( -age / tau )`. So
the decay is exact at any frame count (no per-frame float multiply
compounding), and the GPU's per-frame work is the wedge, not the grid.

### The Over effect

The clip IS the reflectivity: its **brightest channel** (not luma: a saturated
blue ring has luma 0.07 and would never echo) over Threshold, times alpha,
laid under the scope in range and bearing. Scope Size 1 (the Over's default)
makes the scope's circle cover the whole frame; Mix brings the clip back, and
the output alpha is `mix( clip alpha, 1, Mix )` (Resolume's DXV demo clips
carry alpha; a scope painted over a transparent clip is opaque). At Mix 0 the
clip is returned bit for bit (`texelFetch`, see the traps).

### Audio

Resolume's 64 FFT bins through the fleet's primed analyser (`Audio.*`). Every
bin is weighted equally; nothing asks which frequency a bin is, because
nobody has measured the bins. An onset fires a **strobe** (Audio Strobe: the
next wedge painted gets a burst of interference, a radial spoke, the way a
jammer or another radar shows) and, in the source, a **contact** (Audio
Contacts: a strong transient echo 20-60 degrees ahead of the sweep, living
three rotations). The analyser's sensitivity is fixed at the fleet default;
the two controls are amounts, 0 = off. A clock jump (a clip trigger)
resets the analyser, primed: the loud frame it lands on fires nothing.

## Decisions taken without asking

- **Each plugin declares only its own controls, About block last** (`HostOrder()`
  in `Controls.cpp`; host index -> ParamId inside the plugin). Two traps from
  the two-plugin precedents: boreal put the Over group after the About block,
  so the day a user guide exists the new "User guide" button moves the
  effect's indices; flyback declared every control in both plugins, so the
  effect has "inert" controls the gate must be told about. Here a new About
  button appends at the end of both, and there are no inert controls.
  `--names` checks the About block is last.
- **Controls added to the spec's list**: Seed and Land (the source's map),
  Threshold (the Over's clip), Audio Strobe / Audio Contacts (the audio's two
  events, amounts), Heading Line (the spec's "heading marker (optional)"),
  Scope Size (inscribed to covering the frame). **No presets** (the spec asks
  for none; row 1 would only restate the defaults).
- **Defaults**: the source is a harbour approach at dusk: 24 rpm, 2 degrees,
  2 us on a 24 km scope, 0 dB, STC 2.6, seed 11 (a bay open to the north,
  chosen by looking at seeds 1-12), 7 contacts, persistence 1.6 s (so the
  trail visibly fades within the 2.5 s rotation). The Over: STC flat (n = 4),
  +8 dB, clutter 0.2, Scope Size 1, Threshold 0.2, no heading line. Checked on
  the harness's card and on Resolume's bundled demo clips (Trinity_09,
  BattleWeapon_Tank_09, IntoTheGlow_02, Enter5_12, SpaceUniverse_04,
  OrganicMotions_06, Beat 003), still and moving through `--pipe`: the
  picture reads as a radar within the first rotation (2.5 s).
- **Contacts live in scope units, land and rain in km.** A ship at 20 knots
  on a 24 km scope moves a pixel every few seconds; traffic scaled to the
  display is what reads. So Range zooms the coast and leaves the traffic.
- **The shadow is 1.5 km of land**, in km: at 150 km only coastlines show,
  which is right for a surface radar and makes long ranges sparse.
- **One second** is the largest forward clock step taken as elapsed time
  (boreal's is a quarter): a radar at 2 fps should still turn. A frame that
  covers more than a rotation paints each bin once.
- **Provisional About/ATTRIBUTIONS** hand copies with `guide = ""`.

## The traps

Ordered by how much they cost.

**Apple's software renderer's `sin` is not the GPU's.** `--arc` passed on this
Mac's GPU and failed on the software renderer (what a GPU-less CI runner
gets) at Sidelobes 1: the sinc^4 beam 3% wide (FWHM 2.064 against 2.000
degrees), because `sin( x ) / x` there was far enough off. GLSL 4.10 (4.7.1)
bounds +, -, *, / and exp/log and promises nothing for sin, cos, tan or
atan. The shaders now use `sine`, `cosine` and `bearingOf`, built from bounded
operations (a Cody-Waite range reduction and a Taylor series, errors stated
in `kCommon`); `verify.sh` greps the GLSL for the built-ins.

**`1 - exp( -p )` in float is quantisation noise for a faint echo.**
`exp( -1e-5 )` is 0.99999 to one float ulp (6e-8), so a return of 1e-5 came
out with 0.6% noise. `--arc` found it as FWHMs off by 0.1 bin that moved with
the target's range. The video now uses the series below p = 0.01 (exact to
4e-10 relative).

**A full-grid decay pass made the software renderer unusable.** The first
build decayed every one of the 2M phosphor texels every frame (ping-pong):
0.3 ms on this GPU, 0.25 s a frame on the software renderer, so its pass
would have taken most of an hour. The phosphor now stores values at their
last paint and the CPU times the fade per column (see "The phosphor"). A
side effect worth having: no per-frame float multiply compounds.

**Hardware bilinear across columns painted a rotation apart smears.** With
the fade per column, a texture() fetch would interpolate unfaded values and
fade them with the pixel's column: the leading edge of the sweep would carry
last rotation's paint. The composite fetches four texels, fades each, and
interpolates by hand.

**A filtered read at a texel centre is not exact on the software renderer.**
`--over-check`'s "Mix 0 returns the clip bit-exact" passed on the GPU and
failed there (1643 floats). The composite reads the clip with `texelFetch`.

**The arc's first tolerance was derived wrong, twice.** The video-curvature
term was estimated as 5e-4 bins in total; it is (P0 / 8) / |f'| per edge,
~3e-3 bins at P0 = 1e-3 for the 6-degree beam. And a term was missing: the
shader's bearings are floats, the bin's and the target's each good to ~2 ulp
at 2 pi, 1.2e-6 rad, 4e-4 bins per edge. The check failed by 1.3e-4 bins
before that term was written down. Both are derived from ulps and slopes, not
fitted; the peak echo was also cut to P0 = 1e-5, which makes the video term
negligible.

**Screen checks at 320x180 read hairlines.** A 2 us pulse on a 24 km scope is
1 px deep at 320x180, a 2-degree beam at r = 0.4 is 1.2 px across; sampling
along them measured the pixel grid, not the beam. The screen halves of
`--arc` and `--pulse` use an 8 us pulse and an 8-degree beam.

**The own ship ended up in a lake.** fBm's 48 km octave decides whether a whole
24 km scope is land or sea; seeds 3, 4 and 6 put the ship inside a continent,
seen as a bright ring round a black hole. The land is pushed down within
~6 km (in km, so Range still zooms one map), which makes lagoons and bays.

**Luma drops saturated colours.** Resolume's Trinity_09 has a blue ring:
luma 0.07, under any threshold, no echo. The Over uses the brightest channel.

**`PIPESTATUS` is bash's**; the Bash tool's shell here is zsh (`pipestatus`).
`verify.sh` is bash. An exit-status test typed at the zsh prompt printed
nothing, which looks like a pass.

Carried from the fleet and respected here: `ScopedFBOBinding` does not restore
the viewport; allocate before binding (the fade/carry uploads happen before
any pass binds a unit); `FFGLFBO::Release` leaks (PassBuffer); units bound by
hand and released with `unbindTextureUnits`; a sampler bound to texture 0 is
"unloadable" (the source binds its map to the clip unit); `SetTextParameter`
for About; OBJECT library; integer hashing only; the clock-unit vote; primed
onsets; NaN guards on fed-back state; 32-bit float for anything carried; no
`M_PI`, `<cmath>` included, no `far`/`near`; `packed` and the 4.10
reserved-word list; an option reads back 0..1 whatever its count (the sweep
sets options by element index); unique names, as Arena addresses them
(lower case, no spaces).

## Would this hold on another rasteriser, at another raster?

Every numeric check, where its tolerance comes from, and the raster. Every
check runs at 1280x720 and at 320x180 on this Mac's GPU, and at 320x180 on
Apple's software renderer (`verify.sh`). CI runs only `--offline` and glslc:
a macOS runner has no accelerated GL.

| check | bound | why that number | raster / rasteriser |
| --- | --- | --- | --- |
| `--arc`, polar | 2 x (interp + video + float angles), e.g. 0.0070 bins at 6 deg | linear interpolation of the stated pattern, |f''| D^2/8 / |f'| (3e-3 to 1.5e-2 bins/edge); the video's curvature, (P0/8)/|f'| (1e-5); float bearings, 1.2e-6 rad | the phosphor, not a raster: identical at both; passed on the software renderer after `sine` |
| `--arc`, screen | 0.5 px | the harness's bilinear read is a tent of variance 1/6 px^2: +0.17 px on the narrowest (2.7 px) arc; the pixel grid's phase | both rasters, both rasterisers |
| `--pulse` extent | 2e-3 bins | the partial end bins' video curvature, P(1-f)/2 <= 5e-4 of a bin at P = 1e-3, twice; float sums of ~50 terms, 1e-6 | the phosphor: raster-free |
| `--pulse` leading edge | 2e-3 bins | the same | raster-free |
| `--pulse`, screen | 0.5 px | the integral survives bilinear sampling; a 4+ px streak's peak survives a 1 px tent | both rasters |
| `--sweep` | exact integer counts | integer arithmetic on doubles; checked that no frame lands within 1e-6 bins of a bin centre (double accumulation is 1e-12) | counts in float are exact to 2^24: raster- and rasteriser-free |
| `--sweep-law` | exact | the same rule, on the CPU | CPU |
| `--persist` | 8 ulp relative; 1e-12 E absolute where the flash has gone | the fade is a double exp rounded once to float, times a stored value rounded at most three times (paint, carry, add); no per-frame compounding | read through the composite's own fade path (`PolarViewForTest`): the output raster is only the window's size |
| `--persist` repaint | 16 ulp | the ratio of two such values | the same |
| `--r4` slope | 5e-4 | ln(1 - e^-P) = ln P - P/2: points off the line by <= P_max/2 = 5e-4, which moves the slope over ln 8 by <= 2.4e-4 | raster-free |
| `--r4` straightness | 1e-3 in ln | 2 x P_max / 2 | raster-free |
| `--over-check` edges | half a clip pixel + half a range bin | the clip's edge is reconstructed bilinearly (its half level is the pixel boundary; the pulse's midpoint quadrature moves it by at most half a pixel's ramp); the profile is read at bin centres | depends on the raster through the clip pixel: 0.0020 at 1280x720, 0.0063 at 320x180 |
| `--over-check` bearing | 0.01 bin | the squares are placed symmetric about their axis to the pixel, so the centroid is exact up to the float sine/cosine of +-theta | both rasters |
| `--over-check` Mix 0 | bit-exact | `texelFetch`; no arithmetic on the path (mix( a, b, 0 ) = a) | both rasterisers |
| `--prime` | exact counts | a deterministic feed; the analyser is CPU | any |
| `--resize` state | bit-identical | nothing is painted across the resize; the grid is not raster-sized | 1280x720 <-> 320x180 (and 320x180 <-> 160x90 on the software renderer) |
| `--resize` picture | 3 ulp | one frame's fade: a stored value times a double exp rounded to float, each side | the same |
| `--clock` antenna | 1e-6 bins | a double ulp at 4.99e8 ms is 6e-8 ms: a dt good to 1e-10 s, 600 frames of it to 6e-8 bins | raster-free |
| `--clock` dt | 1e-9 s | the same ulp | raster-free |
| `--state`, `--names`, `--cues` | exact | GL state; the 16-byte field; the cue law | raster-free |

**Resize mid-run.** The phosphor, the reflectivity, the kernel and the map are
sized by constants, never by the host's raster, and the per-column timing is
CPU state. `--resize` still runs (and its negative control clears the
phosphor on a resize and must fail), because the brief asks for it and
because the day someone makes a buffer raster-sized it will catch it.

## A check that cannot fail is not a check

`ratest --negative` sets one wrong MODEL at a time -- a test hook in the plugin,
so the shipped shader computes the wrong thing -- and requires the check to
fail. All 11 are detected:

| check | the wrong model |
| --- | --- |
| `--arc` | the one-way pattern (sqrt 2 wider) |
| `--pulse` | a pulse extent of c tau, not c tau / 2 |
| `--sweep` | a line per frame at the antenna's bearing, not the swept wedge (gaps) |
| `--sweep-law` | `Sweep.h`'s `LineAt` in place of `Crossed` |
| `--persist` | no flash: one term, not two |
| `--r4` | the radar equation without its R^4 |
| `--over-check` | the clip laid under the scope mirrored (bearings anticlockwise) |
| `--prime` | the analyser unprimed |
| `--resize` | the phosphor cleared on a resize |
| `--clock` | elapsed time from the host clock as a float (32 ms steps at 499e6 ms) |
| `--cues` | every control ramps between keys |

### The recorded mutation

`tools/mutate.sh` (run by verify.sh) changes one character of shipped code in a
copy of the tree and requires a named check to fail. **The GLSL mutation of
record**: in `beam()`, `float g  = exp( -LN2 * q * q );` became
`exp( -LN2 * q / q )` -- the Gaussian beam a constant 1/2 -- and **`--arc`
caught it** (at Sidelobes 0 the measured FWHM is 0 degrees against 2: no
half-power crossing at all). Two more GLSL mutants
(the pulse running inward, caught by `--pulse`; the composite dividing by the
fade, caught by `--persist`) and one C++ (the wedge starting a bin late,
caught by `--sweep`) are caught too. This proves the harness drives the
shaders the plugin ships, not a copy of them.

## Shape of the code

    source/Controls.*      the ParamIds, HostOrder(), groups, every 0..1 -> units mapping
    source/Sweep.h         the crossing rule: Crossed(), and LineAt() (the negative control)
    source/World.*         the source's contacts (scope units) and rain drift, PCG
    source/Shaders.*       kCommon (sine, cosine, bearingOf, beam, PCG) + kernel, map, reflect, paint, composite
    source/Radar.*         the plugin: parameters, clock, audio, per-column timing, passes
    source/SourcePlugin.cpp, EffectPlugin.cpp   the two registrations
    source/Audio.*         the primed analyser
    tools/ratest/          the harness
    tools/sweep.py, mutate.sh, verify.sh, glslc.sh
    docs/arena-expect.json a DRAFT of the fleet gate's expectation (`ratest --expect`)

## What is genuinely verified, and what is assumed

Verified on this machine (M4 Max, macOS 26), see the README's Status table for
the numbers: every check above on the GPU at two rasters and on the software
renderer at 320x180; both bundles universal; oxbow probe reads `SW Radar` /
`RA01` / source and `SW Radar Over` / `RA02` / effect; oxbow selftest renders
120 frames through each; 43 parameters over both plugins all move the
picture.

Assumed, or not done:

- **Never loaded into Resolume.** Unknown there: how the 28 and 25 parameters
  present, the clock unit, the FFT bins, whether events arrive as 1 then 0.
- **Never built on Windows.** CI and release workflows are adapted from
  flyback and have not run.
- **The phosphor's time constants are stretched** to video rates; the colours
  are from memory of P7/P19/P1, not measured spectra.
- **The radar equation, STC and the pulse are textbook**, cited from memory
  (Skolnik), not re-checked this session. Distributed targets are normalised
  (a decision, above), so the surface's absolute brightness is a look, not a
  radar cross-section.
- **The synthetic sea** (fBm land, rain cells, clutter law, contact speeds) is
  made to look right, not measured.
- **The sweep and bench numbers** were taken while other builds shared the
  machine.
- **No OpenFX port.** The gate expectation in
  `docs/` is a draft nobody has run.

## The browser demo (2026-09-25)

`demo/` is <https://radar-demo.stoatworks-labs.com>, built to the fleet's
`resolume-demo` kit rules. What a reader of it must know:

- **The shaders are the plugin's**, all seven pieces, spliced by
  `demo/tools/check_shaders.py --write` into `demo/shaders.js` and compared
  character for character by the same script, which `tools/verify.sh` runs.
  Negative-controlled once: `q * q` -> `q / q` in the copy fails it. The GLSL's
  bounded `sine`/`cosine`/`bearingOf` stay; WebGL's trig is never used.
- **The CPU half is a port that only a reader checks**: Controls.cpp's
  conversions, `Crossed()`/`Column()`, the per-column Carry/Fade timing in
  double, the clock's jump rule, World.cpp's contacts and rain drift (PCG with
  `Math.imul`), `ringSpacingKm`, `lookOf`, `sincHalfPower`. Same buffer sizes
  and formats (RG32F phosphor, `copyTexSubImage2D` back from the scratch).
- **Differences, all said on the page:** no audio (Audio, Audio Strobe, Audio
  Contacts absent -- with no spectrum the analyser never fires, so nothing
  changes); Contacts and Seed are dropdowns, Seed 0-99; no About block; the
  clock is the page's seconds, the unit vote is not ported; the Plugin switch
  is a new instance with that constructor's defaults (five shared controls
  differ); the Over's clip is the kit's premultiplied one; it needs
  EXT_color_buffer_float and OES_texture_float_linear and refuses without.
- **Seen, not isolated:** headless Chrome on SwiftShader logs an ANGLE
  "GPU stall due to ReadPixels" performance WARNING a few times per browser
  process. The page never calls readPixels; the likeliest source is the
  float `copyTexSubImage2D`. It is not an error and the picture is right.
- **The live page logs one console error that is not the page's**: the zone
  injects an inline `/cdn-cgi/challenge-platform` script and the page's
  `script-src 'self'` CSP blocks it, as on every `*-demo` host (fleet-wide,
  recorded in the demo brief's sweep). Locally there are no errors.

## Open design questions

- Should Beamwidth be the one-way (datasheet) width instead? The arc would then
  be Beamwidth / sqrt 2 wide for the Gaussian.
- Should distributed returns grow with pulse length and beamwidth, as they
  physically do (so Pulse Length is also a gain on land and clutter)?
- Should the Over effect keep the clip's colour in the echoes (a colour
  radar) instead of the phosphor's?
- Sector scan (back and forth over an arc) would suit weather and military
  looks; Direction has only Clockwise and Anticlockwise.
- The source's traffic: ships and aircraft share one speed range in scope units.
