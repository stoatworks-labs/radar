# Attributions

Radar is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Audio analyser and host-clock vote — Stoatworks rosette

<https://github.com/stoatworks-labs/rosette>  
Licence: MIT  
Copyright: Stoatworks Labs

source/Audio.{h,cpp} is boreal's copy of millpond's copy of rosette's analyser (itself from macroblock's), with its primed first frame, which ratest --prime checks. The host-clock unit vote in UpdateClock is rosette's.

### Source-plus-Over shape, GL state and harness — Stoatworks downpour, boreal and flyback

<https://github.com/stoatworks-labs/boreal>  
Licence: MIT  
Copyright: Stoatworks Labs

One core registered as a source and an Over effect is downpour's shape, as boreal and flyback carry it. GLState.h and PassBuffer come from millpond, vectrix and tinsel; the harness shape, the --script cue sheets, tools/sweep.py, tools/mutate.sh and tools/verify.sh from boreal and flyback; the --pipe shapes from pattern (a source) and toner (an effect); the software-renderer pass from plotter.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Work we checked ourselves against

No code was taken from these — but they were how we knew we had it right, and that is worth saying out loud.

### Cathode Ray Tube Phosphors Of Interest To The Experimenter — Patrick Jankowiak (KD5OEI), revision 2010-02-26

<http://www.labguysworld.com/crt_phosphor_research.pdf>

A compilation of EIA/JEDEC TEP116, Whitaker, Tektronix and Dyall (1948) phosphor data, checked 2026-09-25 for the phosphors' character: P7 a dual-colour, dual-persistence cascade for radar indicators (ZnS:Ag blue-white fluorescence over a (Zn,Cd)S:Cu yellow phosphorescence, peaks 440 and 558 nm, over a minute in low ambient light); P19 orange, very long, (KF,MgF2):Mn, radar screens; P1 green, 525 nm, 20 ms. The same table lists P7's long component as an inverse power law, not an exponential; the plugin's afterglow is an exponential with an adjustable time constant, a simplification. TEP116-C itself was not consulted.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### The plan-position indicator

The marine and air-traffic radar display: a CRT whose trace runs out from the centre along the antenna's bearing, on a long-persistence phosphor. Implemented from the textbook radar equation and antenna theory; nothing is copied from anyone's source.

## Standards and published specifications

What the implementation is measured against.

- **The radar range equation and sensitivity time control** — a point target's return falls as 1/R^4; STC is a receiver gain rising with delay (an R^4 law equalises point targets). Checked 2026-09-25 against the Wikipedia article on sensitivity time control and Cambridge Pixel's note on STC; the textbook itself (Skolnik, Introduction to Radar Systems) was not re-read. Area clutter going as R^-3 and volume clutter (rain) as R^-2 follow from the resolution cell's area (R x beamwidth x c tau / 2) and volume.
- **Range resolution c tau / 2** — the echo of a pulse tau long occupies c tau / 2 in range, 150 m per microsecond.
- **Antenna patterns** — the uniform aperture's sinc^2 one-way power pattern, the Gaussian beam, and the two-way pattern as their square: a Gaussian's two-way half-power width is its one-way width over the square root of 2.
- **Melissa E. O'Neill, PCG (2014)** — the integer hash behind every speckle, the land and the traffic.
- **W. J. Cody and W. Waite, Software Manual for the Elementary Functions (1980)** — the two-part pi in the shaders' range reduction.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
