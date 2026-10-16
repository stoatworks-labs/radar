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

## Sprites (radar#5, 2026-10-13)

Steve (VJPandaAU, a working VJ on Windows 11) asked for "sprites as detected
objects": his own image as contacts closing on the centre, with a threshold, a
count, a size, a speed, same-or-varied speeds, steady-or-wandering, upright or
bottom-to-the-centre, hue and saturation, and optionally growth on approach.
Built on `claude/contacts`, released in **v0.2.0** (2026-10-15); in Resolume Arena on Windows
(the gate and a hand check, below), never in Resolume on macOS.

**A sprite is an extended target, not an overlay.** The reflect pass samples
each sprite's mask where it resamples the surface (a second channel, G, of the
now RG32F reflect band), averaged over the same pulse footprint; the paint
convolves both channels with the beam, normalised, and puts the sprites under
the same `r^(n-4)` as the surface (`P = G [ r^(n-4) (surface + sprites + ...) ...]`).
So a sprite is painted by the sweep, smeared by the beam and the pulse exactly as
land is, and fades on the phosphor; between paints it moves, so it leaves the
plot of its track as fading copies. Considered and rejected: drawing the
sprite in the composite (not a radar: no sweep, no smear, no persistence), and
painting it analytically like the point targets (the beam convolution of an
arbitrary picture per texel per tap is the cost the surface path already pays
once). Sprites are not shadowed by land and cast none: they are contacts, like
the ships (`visibility()` multiplies the surface only). Their reflectivity is 1
(`kSpriteLevel`), the brightest coast's.

**The phosphor is four components** (RGBA32F; scratch too): (flash, afterglow)
of everything, then of the sprites' share, carried and faded alike. The share
is the video split by the sprites' share of P, `video * min( mine / p, 1 )`, so
it is never added twice. It exists only so the composite can recolour the
sprites: `light += s.z ( SpriteFlash - Flash ) + s.w ( SpriteAfter - After )`,
skipped outright (`SpriteTint 0`) at Hue 0 / Saturation 0.5. Cost: the buffers go
from ~48 MB to ~88 MB (source; 40 -> 80 the Over). Allocating the share only
when sprites are on would mean reallocating (and losing) the phosphor mid-show.

**Colour, decided:** the sprites start from the phosphor's colours (so P7 stays
two-tone, blue-white flash to yellow-green), Hue turns both about the grey axis
(Rodrigues, keeps the channel mean), Saturation scales chroma about that mean
by 2 x Saturation (0.5 = the phosphor's own). The alternative -- Hue as an
absolute colour -- is easier to aim but makes the sprites look pasted on.

**The mask:** alpha when the picture has real transparency (any alpha < 255),
otherwise the **brightest channel** -- the brief said luma, but the Over already
learned that luma drops saturated colours (Trinity_09's blue ring, 0.07). Hard
threshold (`> Sprite Threshold`): a radar target is there or not, so a sprite is
a silhouette; the bilinear mask under a step gives a smooth contour, and the
beam and pulse antialias it. Box-filtered (area-weighted, mean-preserving) to
at most 128 texels on the longer side, rows bottom first, R32F linear. The
built-in blip is a 64x64 cone, so Threshold T makes it a disc Size x (1 - T)
across: `--sprite-threshold` measures exactly that.

**Loading** (`SpriteImage.cpp`, the only file that sees stb_image or a path):
lazily on the render thread when the path changes, never in the constructor
and never in `SetTextParameter` (which only stores the path, under a mutex, and
must succeed for the empty default the SDK pushes at instantiation).
`STBI_NO_STDIO`: the bytes are read by `ReadFileBytes`, which on Windows widens
the host's UTF-8 to UTF-16 (`Utf8ToUtf16`, strict: overlong forms, surrogates,
> U+10FFFF and truncation are refused) for `_wfopen_s`. downpour's and
jackpot's narrow `fopen` cannot open a non-ASCII path there. A failure (missing,
not a picture, truncated, > 8192 px, > 64 MB) logs the path verbatim and the
reason (`diag::warn`, plus `LogToHost`) and falls back to the blip; it is not
retried until the path changes. `SetFileParamInfo` ONLY (gridiron's trap).

**Motion** (`Swarm.cpp`): every sprite heads for the centre, then a new one
starts at the rim on a hashed bearing (PCG of seed, slot, generation; the Over
has no Seed control and uses 11, the constructor's). Progress is integrated in
double from the host's elapsed time, like the antenna; the wobble's distance is
the closed-form integral of `v ( 1 + a w( t ) )`, so 24 and 240 fps put a sprite
in the same place (`--sprite-motion`: 10^-15), and the weave in bearing is a
function of distance travelled, so a held sprite does not sway. Speed, Speed
Spread and Stability set rates, never positions: a slider never teleports
them. New slots (first frame, or Count raised) start staggered by the golden
ratio, so the scope fills at once and they never arrive in step; respawns drop
the frame's overshoot. Seed re-hashes bearings (positions jump: deliberate).

**Cost and culling.** The CPU hands the reflect pass only the sprites whose
bounding circle the band can reach, with each one's range, radius, bearing and
half-angle; the shader takes the pulse taps only for a texel whose footprint
can reach a sprite. On this GPU 16 sprites are below the bench's noise. On
Apple's software renderer, 12 big sprites cost ~0.13 s a frame at 320x180 (a
first version that looped every sprite per tap, with a 24-float array, was
worse; the array is gone). Note the default SOURCE costs that renderer ~0.4 s a
frame anyway (the coast's shadow), which is why `--sprite-repeat` turns Land off.

**Parameters.** A new group, **Sprites** ("Contacts" is already a control: the
source's ships), declared in both plugins after every v0.1.0 control and before
the About block, so no released control's host index moves (the About block's
do; buttons). Arena stores a composition's values by name, only where they
differ from the default (vectrix `docs/NOTES.md`, proved there), so the order is
not what keeps a saved show working; the NAMES are, and none changed. ParamIds
are internal (`HostOrder()` maps). Names: Sprite File, Sprite Threshold (the
Over's "Threshold" is taken), Sprite Count, Sprite Size, Sprite Speed, Speed
Spread, Stability, Orientation, Hue, Saturation ("Sprite Saturation" is 17
characters), Grow On Approach (16). **Sprite Count defaults to 0**, so a v0.1.0
composition looks exactly as it did: with Count 0 the source renders bit-for-bit
what v0.1.0 did (compared over 600 frames and through `--pipe`); the Over
differs in one 8-bit level at one pixel in 3 of 200 frames (the compiler
reassociating the paint's sum), which no check's tolerance notices.

**Traps hit while building it:**

- A sprite over the centre of the scope paints into EVERY bearing column near
  r = 0, so a radial profile taken down one column had two plateaus and its
  half-maximum walked off the end. `radialExtent()` reads only within reach of
  the sprite, and `isolated()` skips sprites another one (or the centre, or
  the rim) can reach into.
- At 0 dB the tube's 1 - exp( -P ) moves a beam-blurred edge's half level
  outward (by ~0.3 of the blur per edge: 1.3 px on a 68 px square at 720p). The
  geometry checks run at -30 dB, where the video is P.
- `--sprite-repeat` on the default source took 300 s on the software renderer:
  the coast, not the sprites. It runs over open water with clutter and noise.

**Gate (plugin-bench), re-seeded for v0.2.0 (2026-10-14/15):** `plugin-bench/arena/expect/radar.json`
(and its copy `docs/arena-expect.json`) keeps v0.1.0's reviewed rows as they were and adds the
11 sprite rows from `ratest --expect`, with `fixtures: Sprite File <- arena/fixtures/radar-alien.png`
(this repo's alien), Sprite Count probed at 4 and 16 (not 32: llvmpipe), Hue at 0.33/0.67 (0 and 1
are the same turn) and Grow On Approach `declared` (16 characters). `seed_expect.py` against the
running Arena matched it exactly: 40 and 38 host controls, names, order, types, ranges, defaults.
Gate on CI's MSVC DLLs from 67e189c: 17 passed, 0 failed, 2 skipped (audio); source live 12 (Clutter,
Gain, Grow On Approach, Land, Persistence, Phosphor, RPM, Range, STC, Scope Size, Seed, Sprite Size),
Over live 8 (Gain, Mix, Opacity, Persistence, Phosphor, RPM, Scope Size, Threshold), the rest
inconclusive, none dead. Floors 1.55 levels single-grab on the source, 37.8 on the Over. The run
took 37 min (about 15 a plugin). A first run sat silent for 3 h on a stalled ssh tunnel and then
died with it (the Mac had slept): run it under `caffeinate -i` with a tunnel watchdog. The sprites were also driven by hand over REST in SW Radar (blip, PNG, a non-ASCII
folder, a broken file -> blip + WARN, JPG by its brightest channel, Hue/Saturation/Face Centre),
2026-10-13, and looked as the harness says.

**The demo** ports all of it (Swarm, the mask and box filter, the culling, the
tint); Sprite File is a choice of the blip and the two fixtures (copied into
`demo/sprites/`) plus a file button whose picture the browser decodes in place
of stb_image and never sends anywhere. Checked once by eye in the desktop
app's browser (source with aliens, Over with Face Centre pandas, no console
errors), 2026-10-13.

## Sprites since v0.2.0: Path, Start With, Head In, Sprite Colour (2026-10-16)

Steve's reply on radar#5 (2026-10-15) after v0.2.0, decided with Allan: a second path (the radar
moving north through still objects, sprites sliding off the bottom), a "start with" count that
fills the scope over about a crossing, head-first as a third orientation, and the picture's own
colour "if it is trivial". Built on `claude/transit`; **not released, never in Resolume**.

**Parameters**, appended after Grow On Approach and before the About block in both plugins
(`PT_SPRITE_PATH`, `PT_SPRITE_START`, `PT_SPRITE_COLOUR`), so the Sprites group stays one run and
only the About buttons move (source 40 -> 43 declared, Over 37 -> 40):

| name | type | range | default | source host index | Over host index |
| --- | --- | --- | --- | --- | --- |
| Path | option | Closing, Transit | Closing | 35 (new) | 32 (new) |
| Start With | integer | 0..32 | 32 | 36 (new) | 33 (new) |
| Sprite Colour | option | Phosphor, Picture | Phosphor | 37 (new) | 34 (new) |
| Orientation | option | Upright, Face Centre, **Head In** | Upright | 31 (unchanged) | 28 (unchanged) |
| About ... Support the work | | | | 35-39 -> 38-42 | 32-36 -> 35-39 |

**Every default is v0.2.0's behaviour, proved bit for bit**: a v0.2.0 build of `ratest` (from
17951d7) and this one, `--pipe` 640x360, 600 frames, sha-256 of the 8-bit stream, identical for
the source and the Over at their defaults, with 10 blips, 10 aliens, 24 wandering tinted growing
Face Centre aliens, 32 fast pandas, 12 Over aliens and 16 tinted Over blips, and with a cue sheet
raising and lowering Sprite Count mid-run. No shader changed (everything new is CPU-side), the
Closing code in `Swarm.cpp` is the same arithmetic, `drawOf()`'s eight v0.2.0 salts are untouched
(9 and 10 are new), and Start With at 32 places every new slot exactly as v0.2.0 did.

**Transit.** A slot's track is the vertical chord at x0 = 0.85 (2 h - 1), h = PCG of (seed,
slot, generation) salt 10: from y = +sqrt(1 - x0^2) (into range at the top) down to the bottom
rim, length L = 2 sqrt(1 - x0^2), and Speed, Spread and Stability's surge are Closing's, in scope
radii a second, integrated in closed form the same way (`travelled` is distance along the track
on either path). At the end of the track a new generation starts at the top. **Uniform in x, over
the middle 85%**, decided: a slot spends time on a chord in proportion to its length, so uniform
entry fills the band evenly in area, which is what still objects scattered over the sea would give
a ship going north; uniform over the whole width would make edge tracks short scraps half off the
scope (at |x| = 0.95 the chord is 0.62 radii and a 0.1 sprite is half outside all the way). The
band keeps the shortest track 1.05 radii. Considered and rejected: a chord-length-weighted
entry (crowds the middle), and uniform in angle (crowds the edges).

**Transit's weave** is across the track, x = x0 + A 4f (1 - f) sin( 2 pi n s + p ), A = 0.15 (1 -
Stability), f = s / L, n and p Closing's weave draws. The envelope 4f(1 - f) is zero at the rim.
**It keeps the track inside the scope**: the point is inside iff 2 x0 d + d^2 <= s ( L - s ); with
|d| <= A e, e = 4 s ( L - s ) / L^2 <= 1, that holds whenever ( |x0| + A )^2 <= 1, which |x0| <=
0.85 and A <= 0.15 give (equality, a tangent, only at Stability 0 on the band's edge). A held
sprite does not sway (the weave is a function of distance travelled).

**Path is latched per generation**, decided: "a control never teleports a sprite" applies to Path
too, so a sprite in flight finishes on the path it started on and the next takes the new one. The
cost: a change shows only as tracks end (25 s a crossing at the default speed), which the gate will
read as inconclusive or dead, and a VJ may wonder (the guide's "If it looks wrong" says so). The
alternative, re-laying every sprite on the new path at the same fraction along, is a teleport.

**Grow On Approach on Transit** is the same law of range on both paths: Size x ( 1 + 3 g ( 1 - R
) ). On Transit that grows a sprite to its closest approach (R = |x0|) and shrinks it again, and a
track through the middle reaches 4x. Considered: normalising per track so every sprite reaches 4x
at its closest approach (a sprite crossing near the edge would balloon there, and the size would
stop meaning distance from the radar).

**Orientation on Transit** is the bearing rule, unchanged: Face Centre's up is the bearing, Head In
its negative (`d.upSin = faceCentre ? sin : headIn ? -sin : 0`). So on Transit a sprite turns as it
passes the centre (through 180 degrees, quickly if it passes close; a track through the exact
centre flips there). Head In is "top to the centre" on both paths, not "head first along the
track" (which on Transit would be "top to the south"), as Allan specified.

**Start With.** When slots are made (the first frame, or Sprite Count raised), `placeNow = clamp(
Start With - on screen now, 0, new slots )` of them go on at once at v0.2.0's golden-ratio stagger
(StartOf( i ) of the track); the rest are latecomers. **The window** is W = 0.85 x the shortest
track / ( 3^Spread x ( 1 + 0.9 ( 1 - Stability ) ) ), in scope radii at Sprite Speed: the fastest
any sprite can go is Speed x 3^Spread (the top of Spread's range) surging by (1 + a) at most
(|w| <= 1), so a latecomer arriving at t >= 0 cannot leave before t + shortest / that, longer than
W / Speed. The shortest track is 1 radius closing and 2 sqrt( 1 - 0.85^2 ) = 1.054 on Transit. At
the defaults W / Speed is 11.8 s and the quickest exit 13.9 s. **Stratified**, decided: of m
latecomers the j-th waits W ( j + u ) / m, u = PCG salt 9 of (seed, slot, generation 0): an
irregular trickle, never a clump, with the last always in the final m-th. Plain uniform draws were
tried first and seed 11 (the Over's only seed) put the last of nine at 58% of the window, which is
not Steve's "the nth sprite will appear [just before the first leaves]". **A wait is a distance
counted down at Sprite Speed** (not seconds): Speed 0 holds the latecomers back as it holds the
rest still, and a faster Speed brings them in sooner, so the promise survives a Speed change in
flight (not a Spread or Stability raised mid-window). The arrival is exact at any frame rate: the
frame that runs a wait out moves the sprite only for the part of the frame after the moment it ran
out. Respawns still drop the overshoot (v0.2.0's behaviour, which the bit-exact proof needs).

**Sprite Colour.** `SpriteImage` computes, once at load from the full-size picture, the mean of the
stored RGB weighted by the mask value (alpha, or the brightest channel): threshold-free, because
Sprite Threshold is live and the colour is not recomputed. **Encoded values, not linear**: the
composite writes its output with no transfer curve, so the phosphor's colours are already in those
units. HSV saturation under 0.05 is grey (`kGreySaturation`): the panda's mean is 1.7%. On
*Picture* with a coloured picture the composite's sprite colours become that colour scaled to the
channel mean of the phosphor's flash and afterglow colours (`pictureTintOf`), then Hue and
Saturation act on it as on the phosphor's (`tintOf`), so P7's two-tone becomes one colour at two
brightnesses. The blip and grey pictures fall back to Phosphor, and with Phosphor chosen the share
is still skipped outright at Hue 0 / Saturation 0.5. Per-texel colour in the phosphor was decided
against (memory: another two RGBA32F buffers).

**Traps hit:**

- `--sprite-transit`'s per-column bound was first borrowed from `--sprite-speed` (0.25 range bins)
  and the first run read 0.248. Re-derived: each end of a chord is placed to half a bin by sampling
  at bin centres (the 2-tap pulse average splits that step, it does not remove it), so a chord's
  midpoint is good to half a bin. The bound is 0.5; the worst seen is 0.25.
- `--sprites`' orientation bound was 5 degrees ("worst seen 1.3"). A Transit Upright sprite 0.17
  from the centre at 320x180 read 5.41: near the centre the beam's smear is a tenth of a pixel, so
  the pixel grid's point sampling shows. Re-derived for every orientation case: the grid places
  each edge of the lit half to half a pixel, so its centroid is good to atan( 0.5 px / offset ) and
  0.5 px / offset: 6.3 degrees and 11% at 320x180, 3.3 degrees and 6% at 1280x720 (tighter than
  before there).
- The alien's light sits only 5.9% of its box above its middle (3 px at 1280x720, 1.9 at 320x180),
  so it is Head In's second check, not its first: the top-half square is the precise one. A
  320x180 scope has room for one isolated 32-pixel alien, so that raster requires one.
- `--sprite-arrivals` first took 130 s on the software renderer, rendering until the first sprite
  left (24 s of scope). It now renders until the painted count is full and finds the first exit on
  a CPU replica of the plugin's swarm, which must agree with the plugin's to the bit at the last
  rendered frame: 53 s.
- A schedule check that takes each wait from the plugin's own `WaitOf()` cannot catch a wrong
  window (the plugin and the harness move together): the window mutant survived it. Each wait is
  now held to the harness's own statement of W (in its own share of it).
- The browser pane hidden behind another panel never fires `requestAnimationFrame`, so the demo
  never draws; checking it needed a scratch copy of the page with a timer in its place.

## Decisions taken without asking

- **Each plugin declares only its own controls, About block last** (`HostOrder()`
  in `Controls.cpp`; host index -> ParamId inside the plugin). Two traps from
  the two-plugin precedents: boreal put the Over group after the About block,
  so the day a user guide exists the new "User guide" button moves the
  effect's indices; flyback declared every control in both plugins, so the
  effect has "inert" controls the gate must be told about. Here the About
  block is last in both, so the User guide button moved no control (it sits
  second INSIDE the About block, the generator's order, which moved the three
  link buttons down one: harmless before the first tag), and there are no
  inert controls.
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
- **About and attributions are generated** since registration (2026-09-25):
  `StoatworksAbout.h` by `sync-about.py`, `ATTRIBUTIONS.md` by
  `sync-attributions.py`, `.github/FUNDING.yml` by `sync-funding.sh`, the issue
  forms by `sync-issue-templates.py`. The User guide button made it 29 and 26
  parameters. It did NOT append at the end: the generated About block puts it
  SECOND (About, User guide, Project page, Source on GitHub, Support the work), so
  the three link buttons moved down one in both plugins. No control moved, and
  before a first tag that is harmless; the seeded Arena expectation has the real order.

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
| `--sprite-threshold` | 1.5 range bins + the texture's own reconstruction error (0.04-0.16 bins, computed) | half a bin per edge (the edge is sampled at bin centres, and the 2-tap pulse average is a one-bin ramp), a quarter for the 0.5-degree beam averaging neighbouring columns' shorter chords; the bilinear cone's crossing from the harness's own model of the texture | raster-free (the phosphor) |
| `--sprites` size, growth | 1 px on the screen; 1.5 bins on the phosphor | the bilinear read's tent and the pixel grid's phase, at both edges; as above | both rasters |
| `--sprites` orientation | 5 degrees; the offset 25% of a quarter size | the centroid of a sprite at least 18 px across on the pixel grid (worst seen 1.3 deg, 7%) | both rasters, both rasterisers |
| `--sprites` count | exact | connected blobs; sprites spaced by the golden-ratio start | both |
| `--sprite-speed` | 0.25 range bins a paint; slope 1e-3 relative; arrival within one frame | the centroid of a disc's chord sampled at bin centres, its two-tap edges symmetric; the frame is the swarm's step | raster-free |
| `--sprite-beam` | 8 ulp, 16 ulp; share = whole to 4 ulp | as `--persist`; `GainLinear * law * sprite` against `GainLinear * ( law * sprite )`, one reassociation | raster-free |
| `--sprite-repeat` | bit-exact; 1e-9 between 30 and 60 fps | the same operations in the same order; the closed-form integral | both rasterisers |
| `--sprite-colour` | 1e-4 of the brightest channel | float `-ln( 1 - out )` near out = 0.9 is ~1e-6 relative | both |
| `--sprite-motion`, `--sprite-utf16` | 1e-9; exact code units | double sums of frame lengths; integers | CPU (CI runs them) |
| `--sprites` orientation (since v0.2.0) | atan( 0.5 px / offset ), 0.5 px / offset | the pixel grid places each edge of the lit half to half a pixel (replaces "5 deg, 25%: worst seen") | both rasters, both rasterisers |
| `--sprite-transit` columns | 0.5 range bins | each chord end placed to half a bin by bin-centre sampling, so its midpoint to half a bin | raster-free (the phosphor) |
| `--sprite-transit` fit | speed 1e-3 relative; x and the top rim 0.5 bins | a least-squares line through 50-120 columns, each good to half a bin | raster-free |
| `--sprite-transit` weave, grow | 0.5 bins; 1.5 bins | as above; as `--sprites`' growth | both |
| `--sprite-arrivals` count | exact, in rotations with no arrival or exit | connected blobs of one rotation's paint (decay off, the difference of two reads), sprites whose discs touch counted as one | raster-free |
| `--sprite-arrivals` promise | painted full < first exit | the reading lags by up to two rotations (1 s); at 0.06 radii/s the law leaves 1.5 s | both rasterisers |
| `--sprite-colour` Picture | 1e-4 in chromaticity; bit-exact fallback | as `--sprite-colour` | both |
| `--sprite-file` colour | 1e-6 | a float of a double mean of 8-bit values | CPU decode |
| `--sprite-path` | 1e-9; Kolmogorov-Smirnov 1% (1.63 / sqrt n) | double sums of frame lengths; the hash is uniform, KS at 1% | CPU (CI runs it) |

**Resize mid-run.** The phosphor, the reflectivity, the kernel and the map are
sized by constants, never by the host's raster, and the per-column timing is
CPU state. `--resize` still runs (and its negative control clears the
phosphor on a resize and must fail), because the brief asks for it and
because the day someone makes a buffer raster-sized it will catch it.

## A check that cannot fail is not a check

`ratest --negative` sets one wrong MODEL at a time -- a test hook in the plugin,
so the shipped shader computes the wrong thing -- and requires the check to
fail. All 28 are detected (11 since v0.1.0, 9 for the sprites, 8 for the
controls added since v0.2.0):

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
| `--sprite-utf16` | the path widened byte by byte, as a narrow `fopen` reads it |
| `--sprite-motion` | Sprite Speed / 60 added a frame, whatever the frame's length |
| `--sprite-file` | a failed file leaves the last good picture up instead of the blip |
| `--sprite-threshold` | any alpha at all is the target (threshold ignored) |
| `--sprites` | Face Centre drawn upright |
| `--sprite-speed` | the plugin's sprite motion tied to frames (at 24 fps they crawl) |
| `--sprite-beam` | the sprites' share never fades |
| `--sprite-repeat` | the sprites' seed salted from the wall clock |
| `--sprite-colour` | Hue turns the colours the other way |
| `--sprites` | Head In drawn as Face Centre |
| `--sprite-colour` | the picture's colour read as BGR (a DIB's order) |
| `--sprite-path` | Transit's entry points across the whole width, not the middle 85% |
| `--sprite-path` | the latecomers spread over one crossing at Sprite Speed, Spread and the surge ignored |
| `--sprite-transit` | Transit's tracks run north |
| `--sprite-transit` | Transit's sprites grow by the share of the track done, not by range |
| `--sprite-arrivals` | the latecomers over one crossing at Sprite Speed (as above) |
| `--sprite-arrivals` | the sprites' seed salted from the wall clock |

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
shaders the plugin ships, not a copy of them. Since the sprites: the
composite's mutant moved with `glowAt()` (now `* fade` -> `/ fade`, still
`--persist`), and two more are caught: every sprite drawn twice its size
(`2.0 * box.zw` -> `4.0`, `--sprites`) and sprites starting half a radius out
(`1.0 - s.travelled` -> `1.5`, `--sprite-motion`). Since v0.2.0, four more, all C++:
Transit running north (`half - along` -> `half + along`, `--sprite-path`), the
latecomers' window over a crossing (`kArrivalShare = 0.85` -> `1.85`,
`--sprite-arrivals`), Head In mirrored (`-std::sin` -> `+std::sin`, `--sprites`)
and the picture's colour all blue channel (`p[ c ]` -> `p[ 2 ]`, `--sprite-file`).
Ten of ten.

## Shape of the code

    source/Controls.*      the ParamIds, HostOrder(), groups, every 0..1 -> units mapping
    source/Sweep.h         the crossing rule: Crossed(), and LineAt() (the negative control)
    source/World.*         the source's contacts (scope units) and rain drift, PCG
    source/Swarm.*         the sprites' motion: Closing or Transit, latecomers, closed-form wobble, PCG (radar#5)
    source/SpriteImage.*   a file -> the sprite's mask and mean colour (stb_image, _wfopen on Windows), the built-in blip
    source/Shaders.*       kCommon (sine, cosine, bearingOf, beam, PCG) + kernel, map, reflect, paint, composite
    source/Radar.*         the plugin: parameters, clock, audio, per-column timing, passes
    source/SourcePlugin.cpp, EffectPlugin.cpp   the two registrations
    source/Audio.*         the primed analyser
    tools/ratest/          the harness
    tools/fixtures/        sprite fixtures (alien.png/.jpg, panda.png, broken.png) and make_fixtures.py
    external/stb/          stb_image v2.30, the fleet's copy (shunt, flipbook, gridiron, polyhedral)
    tools/sweep.py, mutate.sh, verify.sh, glslc.sh
    docs/arena-expect.json a copy of the fleet gate's expectation (plugin-bench's is the one read)

## What is genuinely verified, and what is assumed

Verified on this machine (M4 Max, macOS 26), see the README's Status table for
the numbers: every check above on the GPU at two rasters and on the software
renderer at 320x180; both bundles universal; oxbow probe reads `SW Radar` /
`RA01` / source and `SW Radar Over` / `RA02` / effect; oxbow selftest renders
120 frames through each; 43 parameters over both plugins all move the
picture (65 since the sprites, 71 since Path, Start With and Sprite Colour,
all of them live).

Assumed, or not done:

- **Never loaded into Resolume on macOS.** Unknown there: how the 29 and 26
  parameters present, the clock unit, the FFT bins, whether events arrive as 1 then 0.
- **Windows, in Resolume Arena 7.27.1** (win-lab, Mesa llvmpipe, no GPU, no sound
  device, 2026-09-25): the DLLs release.yml built from f50bacb (the registered tree)
  load from Extra Effects, register as `SW Radar` / `RA01` / source and `SW Radar
  Over` / `RA02` / effect, all 29 + 27 host controls match the declaration
  (`plugin-bench/arena/expect/radar.json`, seeded by `seed_expect.py` and annotated:
  `requires: audio` on the three audio rows, integer probes for Contacts and Seed,
  Bearing Marks/Heading Line `needs` Scope Size 0 in the Over), both render, Arena's log
  stays clean: 15 of 15 checks, audio skipped, ONE run. Live: source Clutter, Gain,
  Land, Persistence, Phosphor, RPM, Range, STC, Scope Size, Seed; Over Beamwidth, Gain,
  Mix, Opacity, Persistence, Phosphor, RPM, Scope Size, Threshold. Everything else
  INCONCLUSIVE, none dead: the sweep turns between grabs, so the floor is high (1.7
  levels single-grab on the source, 39.7 on the Over, whose scope covers the frame) --
  copperlist's class, the gate's blind spot. MSVC compiled it first time (release.yml
  run 36154527564; the four pre-tag checks were clean: no `packed`/reserved words,
  no `M_PI`, `<cmath>` included, no `far`/`near`, output alpha `mix(clip, 1, Mix)`).
- **The phosphor's time constants are stretched** to video rates; the colours
  are chosen by eye. Checked 2026-09-25 against Patrick Jankowiak's compilation of
  the EIA/JEDEC phosphor tables (labguysworld.com/crt_phosphor_research.pdf, rev.
  2010-02-26): P7 is a dual-colour cascade for radar (ZnS:Ag blue-white fluorescence
  over (Zn,Cd)S:Cu yellow phosphorescence, peaks 440 and 558 nm, over a minute in low
  light), P19 orange (KF,MgF2):Mn, P1 green 525 nm 20 ms. The same table lists P7's
  long component as an INVERSE POWER LAW (after Dyall 1948); the plugin's afterglow
  is an exponential. TEP116-C itself was not consulted.
- **The radar equation, STC and the pulse are textbook**: checked 2026-09-25
  against Wikipedia's sensitivity-time-control article and Cambridge Pixel's STC
  note (1/R^4, a gain rising with range, c tau / 2 = 150 m/us); Skolnik not re-read. Distributed targets are normalised
  (a decision, above), so the surface's absolute brightness is a look, not a
  radar cross-section.
- **The synthetic sea** (fBm land, rain cells, clutter law, contact speeds) is
  made to look right, not measured.
- **The sweep and bench numbers** were taken while other builds shared the
  machine.
- **No OpenFX port.** The user guide is `docs/USER-GUIDE.md` (the only copy anyone
  edits; the PDF and the site page are generated by the website's `build_guides.py`).
  `docs/arena-expect.json` was the build's untested draft (`ratest --expect`, with
  `note` fields the gate does not read); it is now a copy of the gate's real
  expectation, `plugin-bench/arena/expect/radar.json`, seeded by `seed_expect.py` from
  the running Arena and annotated from the constructor. plugin-bench's copy is the
  one the gate reads.
- **Filming the release video** (`stoatworks-backend/video/projects/radar/`) found no
  defect. three facts now in the guide: at the defaults the rain hides behind the land and under the gain (Land 0 and +10 dB show the cells); a Range change under a long afterglow leaves the old map's ghost under the new one for seconds; and a regular beat every half second at 24 rpm is a fifth of a turn, so Audio Strobe's spokes stand still at five bearings.

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

## Design questions settled at release (2026-09-25)

Each kept as built, filmed, and stated as a choice in the user guide:

- **Beamwidth stays the two-way width** (the arc on the screen). The guide tells a
  reader with a datasheet to divide by sqrt 2.
- **Distributed returns stay normalised**: the video's Beamwidth and Pulse Length
  beats show the coast blurring and not brightening, which is what an operator
  wants from those two knobs; the guide says a real radar's land and clutter would
  brighten too.
- **Contacts stay in scope units**: the Range beat shows the coast zoom out while
  the traffic keeps its size; the guide explains why.
- **The Over's echoes stay in the phosphor's colour** (a radar sees reflectivity);
  Mix puts the clip's colour back under them, and the video shows it.

Still open:

- Sector scan (back and forth over an arc) would suit weather and military
  looks; Direction has only Clockwise and Anticlockwise.
- The source's traffic: ships and aircraft share one speed range in scope units.
