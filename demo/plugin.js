/**
 * Radar — browser demo.
 *
 * A plan-position indicator: a CRT whose trace runs out from the centre along
 * the antenna's bearing, while the antenna turns. The one idea, from
 * `AGENTS.md`: **what the scope shows is not the sea, it is the sea convolved
 * with the radar.** A reflectivity field (a synthetic coast in the source, the
 * clip in the Over effect) goes through a radar model — beam, pulse, R^-4, STC,
 * clutter, noise, a two-component phosphor — and the look falls out.
 *
 * Two halves, and they are not equally faithful here:
 *
 *   **The GPU half is the plugin's own GLSL.** `shaders.js` is the seven
 *   `R"( ... )"` bodies of `source/Shaders.cpp` plus kVersion, spliced across
 *   by `demo/tools/check_shaders.py --write` and never typed; the same script
 *   compares them character for character and `tools/verify.sh` runs it. They
 *   are assembled as `Assemble()` assembles them (kVersion + kCommon + body)
 *   and run as the same five passes, in the same order, into buffers of the
 *   same sizes and formats (a 2048 x 1024 RG32F phosphor and scratch, R32F
 *   reflectivity, a 2049 x 1 beam table, a 1024^2 RG32F map), with the same
 *   uniforms as `RadarPlugin::ProcessOpenGL`. The GLSL has no sin, cos or
 *   atan (the plugin's bounded `sine`, `cosine` and `bearingOf`), and nothing
 *   here adds one.
 *
 *   **The CPU half is a port** (`RadarState` below), and nothing checks it but
 *   a reader: the antenna in double and the crossed-bin rule (`Sweep.h`), the
 *   per-column crossing timing that produces the Carry and Fade textures, the
 *   clock's jump rule, the contacts and the rain drift (`World.cpp`), every
 *   0..1 conversion (`Controls.cpp`), the ring spacing, the phosphor colours
 *   and `sincHalfPower`. JavaScript numbers are doubles, as the plugin's CPU
 *   state is, and every value handed to the GPU is rounded to float32 on the
 *   way, as the plugin's is.
 *
 * ---------------------------------------------------------------------------
 * Decisions this page made, and why
 * ---------------------------------------------------------------------------
 *
 * **Both plugins, one page.** `SW Radar` (RA01) is a source and `SW Radar Over`
 * (RA02) an effect: one class with a flag. Unlike flyback, each declares ONLY
 * its own controls (`HostOrder()`), and five shared controls have different
 * defaults (Gain, STC, Clutter, Heading Line, Scope Size). So the Plugin
 * dropdown is a new instance, as it would be in Resolume: the phosphor, the
 * antenna and the sea start again, the panel shows only that plugin's
 * declarations, and every control goes to THAT constructor's default (the
 * Defaults button too). `?plugin=over` opens the effect; Copy link carries it.
 *
 * **Nothing audio.** `Audio` is an FFT buffer Resolume fills; `Audio Strobe`
 * and (source only) `Audio Contacts` are amounts over its onsets. A browser
 * has no Resolume FFT, so all three are absent from the panel rather than
 * present and dead. The removal is exact: with no spectrum the analyser's flux
 * is 0 against a floor of 0.004, it never fires, and neither a strobe nor a
 * transient contact is ever made -- here as there.
 *
 * **Contacts and Seed are dropdowns.** They are FF_TYPE_INTEGER in the plugin
 * (0-16 and 0-9999). The kit has no integer control, so Contacts offers all
 * seventeen values and Seed 0-99 (the default, 11, is among them).
 *
 * **The clock.** The kit's clock is seconds accumulated from frame deltas,
 * capped at 0.1 s a frame, paused by Pause. It is handed to the port as the
 * host's time, already in seconds: the plugin's seconds-or-milliseconds vote
 * against the wall clock is not ported, because this host's unit is known.
 * The jump rule is the plugin's: a step backwards or over a second is a jump,
 * no time passes across it, so Restart leaves the antenna where it was.
 *
 * **The About block is not a parameter here**, as on every page in the suite.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, GLError, bindTexture } from './vendor/gl.js';
import * as S from './shaders.js';

//===========================================================================
// Constants (Controls.h, Radar.cpp, World.cpp)
//===========================================================================

const kPi = 3.14159265358979323846;
const kBearings = 2048;
const kRanges = 1024;
const kMapSize = 1024;
const kMaxTargets = 32;
const kMetresPerMicrosecond = 149.896229;

const kMaxFrameDelta = 1.0;
const kWindowGaussian = 3.0;
const kWindowSidelobes = 8.0;
const kMaxTaps = 128;
const kShadowKm = 1.5;

const kRainPeriod = 2048.0;
const kRainDriftX = 0.012;
const kRainDriftY = -0.008;

const f32 = Math.fround;

//===========================================================================
// Controls.cpp, ported: every host 0..1 to the plugin's units.
//===========================================================================

const clamp01 = (v) => Math.min(1, Math.max(0, v));
const geometric = (v, low, high) => low * Math.pow(high / low, clamp01(v));
const inverseGeometric = (value, low, high) => f32(Math.log(Math.min(high, Math.max(low, value)) / low) / Math.log(high / low));
const linear = (v, low, high) => low + (high - low) * clamp01(v);

const RpmFromParam = (v) => (v <= 0 ? 0 : geometric(v, 1.0, 120.0));
const ParamFromRpm = (rpm) => (rpm <= 0 ? 0 : inverseGeometric(rpm, 1.0, 120.0));
const BeamwidthFromParam = (v) => geometric(v, 0.5, 20.0);
const ParamFromBeamwidth = (d) => inverseGeometric(d, 0.5, 20.0);
const SidelobesFromParam = (v) => clamp01(v);
const PulseFromParam = (v) => geometric(v, 0.05, 20.0);
const ParamFromPulse = (us) => inverseGeometric(us, 0.05, 20.0);
const RangeFromParam = (v) => geometric(v, 0.5, 200.0);
const ParamFromRange = (km) => inverseGeometric(km, 0.5, 200.0);
const GainDbFromParam = (v) => linear(v, -30.0, 50.0);
const ParamFromGainDb = (db) => f32(Math.min(1, Math.max(0, (db + 30.0) / 80.0)));
const StcFromParam = (v) => linear(v, 0.0, 4.0);
const ClutterFromParam = (v) => clamp01(v);
const NoiseFromParam = (v) => clamp01(v);
const RainFromParam = (v) => clamp01(v);
const LandFromParam = (v) => clamp01(v);
const ThresholdFromParam = (v) => clamp01(v);
const PersistenceFromParam = (v) => geometric(v, 0.1, 30.0);
const ParamFromPersistence = (s) => inverseGeometric(s, 0.1, 30.0);
const FlashFromParam = (v) => linear(v, 0.0, 4.0);
/** FlashTauFor: P7, P19, Green. */
const FlashTauFor = (phosphor) => (phosphor === 1 ? 0.025 : phosphor === 2 ? 0.06 : 0.035);
/** std::lround then clamp; lround rounds half away from zero. */
const lround = (v) => (v < 0 ? -Math.round(-v) : Math.round(v));
const OptionIndex = (value, count) => Math.min(count - 1, Math.max(0, lround(value)));

/** Radar.cpp's sincHalfPower(): the y where sinc^4( y ) = 1/2. Newton, once. */
const kSincHalf = (() => {
  const target = Math.pow(0.5, 0.25);
  let y = 1.0;
  for (let i = 0; i < 40; i += 1) {
    const f = Math.sin(y) / y - target;
    const df = (y * Math.cos(y) - Math.sin(y)) / (y * y);
    y -= f / df;
  }
  return y;
})();

/** Radar.cpp's ringSpacingKm(): a round number of km giving four to seven rings. */
function ringSpacingKm(rangeKm) {
  const raw = rangeKm / 5.0;
  const decade = Math.pow(10.0, Math.floor(Math.log10(raw)));
  for (const step of [1.0, 2.0, 2.5, 5.0, 10.0]) {
    if (step * decade >= raw * 0.8) return step * decade;
  }
  return 10.0 * decade;
}

/** Radar.cpp's lookOf(): flash, afterglow, overlay, face, per phosphor. */
const LOOKS = [
  { flash: [0.35, 0.50, 1.10], after: [0.95, 1.10, 0.28], overlay: [0.35, 0.42, 0.18], face: [0.004, 0.009, 0.006] },
  { flash: [1.60, 0.85, 0.30], after: [1.30, 0.55, 0.08], overlay: [0.45, 0.26, 0.06], face: [0.009, 0.005, 0.002] },
  { flash: [0.60, 1.60, 0.70], after: [0.20, 1.20, 0.30], overlay: [0.12, 0.45, 0.16], face: [0.002, 0.010, 0.004] },
];

//===========================================================================
// Sweep.h, ported: the bins the antenna crossed this frame.
//===========================================================================

function Crossed(b0, b1, dt, bins) {
  const w = { first: 0, count: 0, b1Rel: 0, ageScale: 0 };
  if (b1 === b0) return w;
  let first;
  let last;
  if (b1 > b0) {
    first = Math.floor(b0) + 1;
    last = Math.floor(b1);
    if (last - first + 1 > bins) first = last - bins + 1;
  } else {
    first = Math.ceil(b1);
    last = Math.ceil(b0) - 1;
    if (last - first + 1 > bins) last = first + bins - 1;
  }
  if (last < first) return w;
  w.first = first;
  w.count = last - first + 1;
  w.b1Rel = b1 - first;
  w.ageScale = dt / (b1 - b0);
  return w;
}

/** k mod bins, non-negative. */
const Column = (k, bins) => ((k % bins) + bins) % bins;

//===========================================================================
// World.cpp, ported: the source's contacts and the rain's drift.
//===========================================================================

/** PCG, uint32 throughout (= Shaders.cpp pcg()). */
function Pcg(v) {
  const state = (Math.imul(v >>> 0, 747796405) + 2891336453) >>> 0;
  const word = Math.imul(((state >>> ((state >>> 28) + 4)) ^ state) >>> 0, 277803737) >>> 0;
  return ((word >>> 22) ^ word) >>> 0;
}
const Hash01 = (v) => Pcg(v) / 4294967296.0;
const u32 = (v) => v >>> 0;

class Sea {
  constructor() {
    this.contacts = [];
    this.lastSeed = 0xffffffff;
    this.rainX = 0.0;
    this.rainY = 0.0;
  }

  spawn(seed, index, generation, atRim) {
    const base = Pcg(u32(Math.imul(seed, 7919) + Math.imul(index, 104729) + Math.imul(generation, 1299709) + 17));
    const h = (salt) => Hash01(u32(base + Math.imul(salt, 2654435761)));
    const c = { x: 0, y: 0, vx: 0, vy: 0, sigma: 1, generation };
    const bearing = 2.0 * kPi * h(1);
    let heading;
    if (atRim) {
      c.x = Math.sin(bearing);
      c.y = Math.cos(bearing);
      heading = bearing + kPi + (h(2) - 0.5) * 1.4;
    } else {
      const r = 0.2 + 0.75 * Math.sqrt(h(3));
      c.x = r * Math.sin(bearing);
      c.y = r * Math.cos(bearing);
      heading = 2.0 * kPi * h(2);
    }
    const speed = 0.003 + 0.009 * h(4);
    c.vx = speed * Math.sin(heading);
    c.vy = speed * Math.cos(heading);
    c.sigma = f32(1.0 + 5.0 * h(5) * h(6));
    return c;
  }

  setCount(count, seed) {
    count = Math.max(count, 0);
    if (seed !== this.lastSeed) {
      this.contacts = [];
      this.lastSeed = seed;
    }
    while (this.contacts.length > count) this.contacts.pop();
    while (this.contacts.length < count) this.contacts.push(this.spawn(seed, this.contacts.length, 0, false));
  }

  step(dt, seed) {
    for (let i = 0; i < this.contacts.length; i += 1) {
      const c = this.contacts[i];
      c.x += c.vx * dt;
      c.y += c.vy * dt;
      if (c.x * c.x + c.y * c.y > 1.02 * 1.02) this.contacts[i] = this.spawn(seed, i, c.generation + 1, true);
    }
    // Transients (audio contacts) are never made here: no spectrum, no onset.
  }

  driftRain(dt) {
    this.rainX = (this.rainX + kRainDriftX * dt) % kRainPeriod;
    this.rainY = (this.rainY + kRainDriftY * dt) % kRainPeriod;
  }

  targets() {
    const out = [];
    for (const c of this.contacts) {
      if (out.length >= kMaxTargets) break;
      const r = Math.sqrt(c.x * c.x + c.y * c.y);
      if (r > 1.02) continue;
      let bearing = Math.atan2(c.x, c.y);
      if (bearing < 0.0) bearing += 2.0 * kPi;
      out.push({ bearing: f32(bearing), range: f32(r), sigma: c.sigma });
    }
    return out;
  }
}

//===========================================================================
// The plugin's per-instance state and ProcessOpenGL, ported.
//===========================================================================

function floatTexture(gl) {
  const t = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, t);
  gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RG32F, kBearings, 1);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  gl.bindTexture(gl.TEXTURE_2D, null);
  return t;
}

/** One instance: what `RadarPlugin` holds between frames. */
class RadarState {
  constructor(gl, isEffect) {
    this.gl = gl;
    this.isEffect = isEffect;

    // PassBuffer::Ensure's formats and sampling. WebGL2 zero-initialises a
    // new texture, which is the plugin's "newly allocated buffers are cleared".
    this.phosphor = new PassBuffer(gl, { filter: 'nearest' }).ensure(kBearings, kRanges, gl.RG32F);
    this.scratch = new PassBuffer(gl, { filter: 'nearest' }).ensure(kBearings, kRanges, gl.RG32F);
    this.reflect = new PassBuffer(gl, { filter: 'nearest' }).ensure(kBearings, kRanges, gl.R32F);
    this.kernel = new PassBuffer(gl, { filter: 'nearest' }).ensure(kBearings + 1, 1, gl.R32F);
    this.map = isEffect ? null : new PassBuffer(gl, { filter: 'linear' }).ensure(kMapSize, kMapSize, gl.RG32F);
    this.fadeTexture = floatTexture(gl);
    this.carryTexture = floatTexture(gl);

    this.kernelHalfBeam = -1;
    this.kernelSidelobes = -1;
    this.mapRange = -1;
    this.mapLand = -1;
    this.mapSeed = 0xffffffff;

    this.antenna = -0.5;
    this.clock = 0.0;
    this.lastPaint = new Float64Array(kBearings).fill(-1.0e300);
    this.fade = new Float32Array(kBearings * 2);
    this.carry = new Float32Array(kBearings * 2);
    this.lastNow = -1.0;

    this.sea = new Sea();
    this.stats = { rotations: 0, painted: 0, targets: 0, rpm: 0 };
  }

  dispose() {
    const gl = this.gl;
    for (const b of [this.phosphor, this.scratch, this.reflect, this.kernel, this.map]) b?.dispose();
    gl.deleteTexture(this.fadeTexture);
    gl.deleteTexture(this.carryTexture);
  }
}

function createRenderer(gl, quad) {
  // The map is RG32F sampled bilinearly (Sampling::Linear). Without this
  // extension a float texture with LINEAR filtering is incomplete and reads
  // as black: a coastline that silently is not there.
  if (!gl.getExtension('OES_texture_float_linear')) {
    throw new GLError('OES_texture_float_linear is missing. The source’s map is a 32-bit float texture read bilinearly, as in the plugin; without the extension the coast would silently read as open sea.');
  }

  // Assemble(): kVersion + kCommon + the piece. The vertex stage is kVersion +
  // kQuadVertex, as InitGL builds it.
  const vertex = S.VERSION + S.QUAD_VERTEX;
  const assemble = (body) => S.VERSION + S.COMMON + body;
  const programs = {
    kernel: new Program(gl, vertex, assemble(S.KERNEL_FRAGMENT), 'kernel'),
    map: new Program(gl, vertex, assemble(S.MAP_FRAGMENT), 'map'),
    reflect: new Program(gl, vertex, assemble(S.REFLECT_FRAGMENT), 'reflect'),
    paint: new Program(gl, vertex, assemble(S.PAINT_FRAGMENT), 'paint'),
    composite: new Program(gl, vertex, assemble(S.COMPOSITE_FRAGMENT), 'composite'),
  };

  let instance = null;
  let instanceVariant = null;

  const unbind = (count) => {
    for (let unit = count - 1; unit >= 0; unit -= 1) bindTexture(gl, unit, null);
    gl.activeTexture(gl.TEXTURE0);
  };

  function setBeamUniforms(program, params) {
    const half = (0.5 * BeamwidthFromParam(params.get('beamwidth')) * kPi) / 180.0;
    program.set('BeamHalf', f32(half));
    program.set('SincK', f32(kSincHalf / half));
    program.set('Sidelobes', f32(SidelobesFromParam(params.get('sidelobes'))));
    program.setInt('OneWayForTest', 0);
  }

  /** RadarPlugin::buildMap. */
  function buildMap(st, params) {
    const rangeKm = f32(RangeFromParam(params.get('range')));
    const land = f32(LandFromParam(params.get('land')));
    const seed = Math.min(9999, Math.max(0, lround(params.get('seed'))));
    if (rangeKm === st.mapRange && land === st.mapLand && seed === st.mapSeed) return;
    st.mapRange = rangeKm;
    st.mapLand = land;
    st.mapSeed = seed;

    gl.bindFramebuffer(gl.FRAMEBUFFER, st.map.fbo);
    gl.viewport(0, 0, kMapSize, kMapSize);
    const p = programs.map.use();
    p.set('RangeKm', rangeKm);
    p.set('LandAmount', land);
    p.set('MapSize', kMapSize);
    p.setUint('MapSeed', Pcg(u32(Math.imul(seed, 31) + 7)));
    quad.draw();
  }

  return {
    get stats() { return instance?.stats ?? null; },

    render({ input, params, width, height, time, variant }) {
      const isEffect = variant === 'over';
      if (!instance || instanceVariant !== variant) {
        instance?.dispose();
        instance = new RadarState(gl, isEffect);
        instanceVariant = variant;
      }
      const st = instance;
      gl.disable(gl.BLEND);

      //-------------------------------------------------------------------
      // Time: the host's (the page's clock, in seconds), frame to frame. A
      // backwards or large step is a jump and no time passes across it.
      //-------------------------------------------------------------------
      const now = time;
      let dt = 0.0;
      if (st.lastNow >= 0.0) {
        const step = now - st.lastNow;
        if (!(step < 0.0 || step > kMaxFrameDelta)) dt = step;
        // (a jump also resets the audio analyser; there is none here)
      }
      st.lastNow = now;

      //-------------------------------------------------------------------
      // The antenna, in bins, in double; the wedge it swept.
      //-------------------------------------------------------------------
      const rpm = RpmFromParam(params.get('rpm'));
      const direction = OptionIndex(params.get('direction'), 2) === 1 ? -1 : 1;
      const rate = (rpm / 60.0) * kBearings * direction;
      const b0 = st.antenna;
      st.antenna += rate * dt;
      const wedge = Crossed(b0, st.antenna, dt, kBearings);

      //-------------------------------------------------------------------
      // The sea.
      //-------------------------------------------------------------------
      const seed = Math.min(9999, Math.max(0, lround(isEffect ? 11 : params.get('seed'))));
      if (!isEffect) {
        st.sea.setCount(Math.min(16, Math.max(0, lround(params.get('contacts')))), seed);
        st.sea.step(dt, seed);
        st.sea.driftRain(dt);
      }
      const targets = isEffect ? [] : st.sea.targets();

      //-------------------------------------------------------------------
      // The scope on the screen.
      //-------------------------------------------------------------------
      const scopeSize = Math.min(1, Math.max(0, params.get('scopeSize')));
      const inscribed = f32(0.95 * 0.5 * Math.min(width, height));
      const covering = f32(0.5 * Math.sqrt(width * width + height * height));
      const scopeRadius = f32(inscribed + (covering - inscribed) * scopeSize);
      const centre = [f32(0.5 * width), f32(0.5 * height)];

      if (!isEffect) buildMap(st, params);

      const rangeKm = RangeFromParam(params.get('range'));
      const pulseExtent = (0.5 * 2.0 * kMetresPerMicrosecond * PulseFromParam(params.get('pulse'))) / 1000.0 / rangeKm;
      const beamHalf = (0.5 * BeamwidthFromParam(params.get('beamwidth')) * kPi) / 180.0;
      const sidelobes = SidelobesFromParam(params.get('sidelobes'));
      const binAngle = (2.0 * kPi) / kBearings;
      const win = Math.min(Math.ceil(((sidelobes > 0.0 ? kWindowSidelobes : kWindowGaussian) * beamHalf) / binAngle), kBearings / 2 - 1);
      const stride = Math.max(1, Math.trunc((win + kMaxTaps - 1) / kMaxTaps));

      //-------------------------------------------------------------------
      // 0. The beam's table, when the beam has changed.
      //-------------------------------------------------------------------
      if (f32(beamHalf) !== st.kernelHalfBeam || f32(sidelobes) !== st.kernelSidelobes) {
        st.kernelHalfBeam = f32(beamHalf);
        st.kernelSidelobes = f32(sidelobes);
        gl.bindFramebuffer(gl.FRAMEBUFFER, st.kernel.fbo);
        gl.viewport(0, 0, kBearings + 1, 1);
        const p = programs.kernel.use();
        p.setInt('Half', kBearings / 2);
        p.setInt('Bearings', kBearings);
        setBeamUniforms(p, params);
        quad.draw();
      }

      //-------------------------------------------------------------------
      // 1. Reflectivity, for the band the beam reaches from this wedge.
      //-------------------------------------------------------------------
      const threshold = ThresholdFromParam(params.get('threshold'));
      const surfaceOn = isEffect ? threshold < 1.0 : LandFromParam(params.get('land')) > 0.0;
      if (wedge.count > 0 && surfaceOn) {
        gl.bindFramebuffer(gl.FRAMEBUFFER, st.reflect.fbo);
        const p = programs.reflect.use();
        bindTexture(gl, 0, isEffect ? input.texture : st.map.texture);
        p.setSampler('MapTexture', 0);
        p.setInt('IsEffect', isEffect ? 1 : 0);
        p.setInt('Bearings', kBearings);
        p.setInt('Ranges', kRanges);
        p.set('PulseExtent', f32(pulseExtent));
        p.set('ClipCentre', centre[0], centre[1]);
        p.set('ClipRadius', scopeRadius);
        p.set('MirrorForTest', 1.0);
        p.set('ClipRaster', width, height);
        // GetMaxGLTexCoords: the page's clip textures are exactly the raster.
        p.set('MaxUV', 1.0, 1.0);
        p.set('Threshold', f32(threshold));
        p.set('ShadowScale', f32(rangeKm / kShadowKm));

        const total = wedge.count + 2 * win;
        if (total >= kBearings) {
          gl.viewport(0, 0, kBearings, kRanges);
          quad.draw();
        } else {
          const start = Column(wedge.first - win, kBearings);
          const first = Math.min(total, kBearings - start);
          gl.viewport(start, 0, first, kRanges);
          quad.draw();
          if (first < total) {
            gl.viewport(0, 0, total - first, kRanges);
            quad.draw();
          }
        }
        unbind(1);
      }

      //-------------------------------------------------------------------
      // 2. Time the paint, in double: when each crossed column was crossed,
      // the decay each carries from its last crossing, and every column's
      // fade from its last crossing to now.
      //-------------------------------------------------------------------
      const kind = OptionIndex(params.get('phosphor'), 3);
      const tauF = FlashTauFor(kind);
      const tauA = PersistenceFromParam(params.get('persistence'));
      st.clock += dt;
      for (let i = 0; i < wedge.count; i += 1) {
        const col = Column(wedge.first + i, kBearings);
        const crossed = st.clock - Math.max((wedge.b1Rel - i) * wedge.ageScale, 0.0);
        const since = crossed - st.lastPaint[col];
        const never = st.lastPaint[col] < -1.0e299;
        st.carry[col * 2 + 0] = never ? 0.0 : Math.exp(-since / tauF);
        st.carry[col * 2 + 1] = never ? 0.0 : Math.exp(-since / tauA);
        st.lastPaint[col] = crossed;
      }
      for (let col = 0; col < kBearings; col += 1) {
        const age = st.clock - st.lastPaint[col];
        const never = st.lastPaint[col] < -1.0e299;
        st.fade[col * 2 + 0] = never ? 0.0 : Math.exp(-age / tauF);
        st.fade[col * 2 + 1] = never ? 0.0 : Math.exp(-age / tauA);
      }
      for (const [texture, data] of [[st.fadeTexture, st.fade], [st.carryTexture, st.carry]]) {
        gl.bindTexture(gl.TEXTURE_2D, texture);
        gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, kBearings, 1, gl.RG, gl.FLOAT, data);
      }
      gl.bindTexture(gl.TEXTURE_2D, null);

      //-------------------------------------------------------------------
      // 3. Paint the wedge into the scratch buffer, and copy its columns back.
      //-------------------------------------------------------------------
      if (wedge.count > 0) {
        const start = Column(wedge.first, kBearings);
        const first = Math.min(wedge.count, kBearings - start);
        const rects = [[start, first], [0, wedge.count - first]];

        gl.bindFramebuffer(gl.FRAMEBUFFER, st.scratch.fbo);
        const p = programs.paint.use();
        bindTexture(gl, 0, st.phosphor.texture);
        bindTexture(gl, 1, st.reflect.texture);
        bindTexture(gl, 2, st.carryTexture);
        bindTexture(gl, 3, st.kernel.texture);
        p.setSampler('Previous', 0);
        p.setSampler('Reflect', 1);
        p.setSampler('Carry', 2);
        p.setSampler('Kernel', 3);
        p.setInt('KernelHalf', kBearings / 2);
        p.setInt('SurfaceOn', surfaceOn ? 1 : 0);
        p.setInt('Bearings', kBearings);
        p.setInt('Ranges', kRanges);
        p.setInt('FirstCol', start);
        p.setUint('FirstSerial', wedge.first >>> 0);
        p.set('Weights', f32(FlashFromParam(params.get('flash'))), 1.0);
        p.setInt('CountForTest', 0);
        p.setInt('Window', win);
        p.setInt('Stride', stride);
        setBeamUniforms(p, params);
        p.set('GainLinear', f32(Math.pow(10.0, GainDbFromParam(params.get('gain')) / 10.0)));
        p.set('Stc', f32(StcFromParam(params.get('stc'))));
        p.set('RangeExponent', 4.0);
        p.set('PulseExtent', f32(pulseExtent));

        const targetData = new Float32Array(kMaxTargets * 4);
        const targetCount = Math.min(targets.length, kMaxTargets);
        for (let i = 0; i < targetCount; i += 1) {
          targetData[i * 4 + 0] = targets[i].bearing;
          targetData[i * 4 + 1] = targets[i].range;
          targetData[i * 4 + 2] = targets[i].sigma;
        }
        p.setArray('Targets', targetData, 4);
        p.setInt('TargetCount', targetCount);

        const clutter = ClutterFromParam(params.get('clutter'));
        p.set('ClutterLevel', f32(1.2 * clutter));
        p.set('ClutterKm', f32(0.6 + 4.0 * clutter));
        p.set('NoiseLevel', f32(0.04 * NoiseFromParam(params.get('noise'))));
        p.set('RainLevel', isEffect ? 0.0 : f32(0.25 * RainFromParam(params.get('rain'))));
        p.set('RangeKm', f32(rangeKm));
        p.set('RainOffset', f32(st.sea.rainX), f32(st.sea.rainY));
        p.setUint('Seed', Pcg(u32(Math.imul(seed, 2654435761) + 99)));
        // pendingStrobe: only an audio onset sets it, and there is none.
        p.set('StrobeLevel', 0.0);
        for (const [x, w] of rects) {
          if (w > 0) {
            gl.viewport(x, 0, w, kRanges);
            quad.draw();
          }
        }
        unbind(4);

        // The scratch buffer is the read framebuffer now: its wedge columns go
        // back over the phosphor's.
        gl.bindTexture(gl.TEXTURE_2D, st.phosphor.texture);
        for (const [x, w] of rects) {
          if (w > 0) gl.copyTexSubImage2D(gl.TEXTURE_2D, 0, x, 0, x, 0, w, kRanges);
        }
        gl.bindTexture(gl.TEXTURE_2D, null);
      }

      //-------------------------------------------------------------------
      // 4. Composite, into the canvas.
      //-------------------------------------------------------------------
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, width, height);
      {
        const look = LOOKS[kind];
        const p = programs.composite.use();
        bindTexture(gl, 0, st.phosphor.texture);
        bindTexture(gl, 1, st.fadeTexture);
        bindTexture(gl, 2, isEffect ? input.texture : st.map.texture);
        p.setSampler('Phosphor', 0);
        p.setSampler('Fade', 1);
        p.setSampler('InputTexture', 2);
        p.setInt('Bearings', kBearings);
        p.setInt('Ranges', kRanges);
        p.setInt('PolarViewForTest', 0);
        const origin = p.location('PolarOrigin');
        if (origin !== null) gl.uniform2i(origin, 0, 0);
        p.set('Raster', width, height);
        p.set('ViewOrigin', 0, 0);
        p.set('Centre', centre[0], centre[1]);
        p.set('RadiusPx', scopeRadius);
        p.set('MaxUV', 1.0, 1.0);
        p.set('FlashColour', ...look.flash);
        p.set('AfterColour', ...look.after);
        p.set('OverlayColour', ...look.overlay);
        p.set('FaceColour', ...look.face);
        p.set('RingSpacing', f32(ringSpacingKm(rangeKm) / rangeKm));
        p.set('RingLevel', Math.min(1, Math.max(0, params.get('rings'))));
        p.set('MarkLevel', Math.min(1, Math.max(0, params.get('bearingMarks'))));
        p.setInt('HeadingLine', params.get('headingLine') > 0.5 ? 1 : 0);
        p.setInt('IsEffect', isEffect ? 1 : 0);
        p.set('MixAmount', isEffect ? Math.min(1, Math.max(0, params.get('mix'))) : 1.0);
        quad.draw();
        unbind(3);
      }

      st.stats = {
        rpm,
        rotations: st.antenna / kBearings,
        painted: wedge.count,
        targets: targets.length,
        rangeKm,
        ringKm: ringSpacingKm(rangeKm),
        clock: st.clock,
      };
    },
  };
}

//===========================================================================
// The parameters: each constructor's, in HostOrder()'s order and groups,
// with the plugin's names and defaults. `only` says which plugin declares it.
//===========================================================================

const fmt = (n, digits = 0) => n.toFixed(digits);
const signed = (n, digits = 0) => `${n >= 0 ? '+' : '−'}${Math.abs(n).toFixed(digits)}`;
/** Three significant figures, for the geometric controls. */
const sig = (n) => (n >= 100 ? n.toFixed(0) : n >= 10 ? n.toFixed(1) : n >= 1 ? n.toFixed(2) : n.toFixed(3));

/**
 * The constructor's defaults: its whole `params[ ... ] = ...` block, source then
 * Over. Each holds every id, because the plugin's array does: a control the
 * Over does not declare still has its value there (Seed is 11 in both).
 */
const DEFAULTS = {
  source: {
    rpm: ParamFromRpm(24.0), beamwidth: ParamFromBeamwidth(2.0), sidelobes: 0.35, direction: 0,
    pulse: ParamFromPulse(2.0), range: ParamFromRange(24.0), gain: ParamFromGainDb(0.0), stc: 0.65,
    clutter: 0.4, noise: 0.3, contacts: 7, rain: 0.4, land: 0.7, seed: 11,
    persistence: ParamFromPersistence(1.6), flash: 0.375, phosphor: 0, rings: 0.35, bearingMarks: 0.5,
    headingLine: 1, scopeSize: 0.0,
    threshold: 0.2, mix: 1.0,
  },
  over: {
    rpm: ParamFromRpm(24.0), beamwidth: ParamFromBeamwidth(2.0), sidelobes: 0.35, direction: 0,
    pulse: ParamFromPulse(2.0), range: ParamFromRange(24.0), gain: ParamFromGainDb(8.0), stc: 1.0,
    clutter: 0.2, noise: 0.3, contacts: 7, rain: 0.4, land: 0.7, seed: 11, threshold: 0.2,
    persistence: ParamFromPersistence(1.6), flash: 0.375, phosphor: 0, rings: 0.35, bearingMarks: 0.5,
    headingLine: 0, scopeSize: 1.0, mix: 1.0,
  },
};

const query = new URLSearchParams(window.location.search);
const initialVariant = query.get('plugin') === 'over' ? 'over' : 'source';

const std = (id, name, group, display, hint, only) => ({ id, name, type: 'standard', group, display, hint, only });
const opt = (id, name, elements, group, hint, only) => ({ id, name, type: 'option', elements, group, hint, only });

const SEED_CHOICES = 100;

const PARAMS = [
  std('rpm', 'RPM', 'Antenna', (v) => (RpmFromParam(v) === 0 ? 'stopped' : `${sig(RpmFromParam(v))} rpm`),
    'Antenna revolutions per minute: exactly 0 at the bottom (stopped: nothing is painted and the picture fades), then 1 to 120, geometrically.'),
  std('beamwidth', 'Beamwidth', 'Antenna', (v) => `${sig(BeamwidthFromParam(v))}°`,
    'The two-way half-power width, 0.5° to 20°: the width of the arc a point target paints. A one-way datasheet beamwidth is √2 wider for the Gaussian.'),
  std('sidelobes', 'Sidelobes', 'Antenna', (v) => fmt(SidelobesFromParam(v), 2),
    '0 is a Gaussian beam with none; 1 a uniform aperture’s sinc⁴, scaled to the same half-power width, so the arc’s width does not move.'),
  opt('direction', 'Direction', ['Clockwise', 'Anticlockwise'], 'Antenna'),

  std('pulse', 'Pulse Length', 'Transmitter', (v) => `${sig(PulseFromParam(v))} µs`,
    '0.05 to 20 µs. A point target paints a radial streak c τ / 2 long, outward from its range, and the surface is smeared outward by the same footprint.'),
  std('range', 'Range', 'Transmitter', (v) => `${sig(RangeFromParam(v))} km`,
    'The scope’s radius, 0.5 to 200 km. The coast and the rain are in km, so this zooms them; the contacts are in scope units and stay put.'),
  std('gain', 'Gain', 'Transmitter', (v) => `${signed(GainDbFromParam(v), 1)} dB`, 'Receiver gain, −30 to +50 dB. The radar equation is normalised to 1 at the scope’s edge, so a dB means the same at any Range.'),
  std('stc', 'STC', 'Transmitter', (v) => `n ${fmt(StcFromParam(v), 2)}`,
    'Sensitivity time control: the gain rises as Rⁿ, n 0 to 4. At n = 4 the R⁻⁴ of the radar equation is flat; lower lets the clutter crowd the centre.'),

  std('clutter', 'Clutter', 'Returns', (v) => fmt(ClutterFromParam(v), 2), 'Sea clutter: area-extensive, falling with range over 0.6 + 4 × this km, speckled pulse to pulse.'),
  std('noise', 'Noise', 'Returns', (v) => fmt(NoiseFromParam(v), 2), 'Receiver noise, before the STC gain, so STC pushes it down near the centre too.'),
  opt('contacts', 'Contacts', Array.from({ length: 17 }, (_, i) => String(i)), 'Returns',
    'Moving point targets, painted analytically. FF_TYPE_INTEGER 0–16 in the plugin; a dropdown of the same seventeen values here.', 'source'),
  std('rain', 'Rain', 'Returns', (v) => fmt(RainFromParam(v), 2), 'Drifting rain cells: volume-extensive, speckled.', 'source'),
  std('land', 'Land', 'Returns', (v) => fmt(LandFromParam(v), 2), 'Open sea to an archipelago. The coast facing the radar is a bright edge; the land behind it falls into shadow.', 'source'),
  opt('seed', 'Seed', Array.from({ length: SEED_CHOICES }, (_, i) => String(i)), 'Returns',
    'Which coast and which traffic. FF_TYPE_INTEGER 0–9999 in the plugin; 0–99 here.', 'source'),
  std('threshold', 'Threshold', 'Returns', (v) => fmt(ThresholdFromParam(v), 2),
    'The clip’s brightest channel (not its luma), times alpha, below which nothing echoes.', 'over'),

  std('persistence', 'Persistence', 'Scope', (v) => `${sig(PersistenceFromParam(v))} s`, 'The afterglow’s time constant, 0.1 to 30 s. The trail behind the sweep fades by it.'),
  std('flash', 'Flash', 'Scope', (v) => `${fmt(FlashFromParam(v), 2)}×`, 'The flash’s strength against the afterglow’s 1: the bright leading edge of the sweep. Its time constant is the phosphor’s, stretched to a few frames.'),
  opt('phosphor', 'Phosphor', ['P7', 'P19', 'Green'], 'Scope', 'P7: blue-white flash, yellow-green afterglow. P19: orange. Green: a green screen.'),
  std('rings', 'Rings', 'Scope', (v) => fmt(v, 2), 'Range rings, a round number of km apart (four to seven of them).'),
  std('bearingMarks', 'Bearing Marks', 'Scope', (v) => fmt(v, 2), 'Ticks round the rim every 5°, longer at 10° and 30°.'),
  { id: 'headingLine', name: 'Heading Line', type: 'boolean', group: 'Scope', hint: 'A line from the centre to north: the ship’s heading.' },
  std('scopeSize', 'Scope Size', 'Scope', (v) => fmt(v, 2), '0: the scope inscribed in the frame; 1: covering the whole frame.'),
  std('mix', 'Mix', 'Scope', (v) => fmt(v, 2), 'The clip against the scope. At 0 the clip is returned as it came.', 'over'),
];
for (const p of PARAMS) p.default = DEFAULTS[initialVariant][p.id];

let renderer = null;

const mounted = mountDemo({
  name: 'Radar',
  pluginId: 'RA01 · RA02',
  kind: ['source', 'effect'],
  tagline:
    'A plan-position radar scope. The antenna turns, each echo brightens a long-persistence phosphor at its range and bearing, and nothing is drawn as a picture of a radar: a reflectivity field goes through a radar model and the look falls out — arcs as wide as the beam, streaks c τ / 2 long, clutter crowding the centre, coasts that are bright edges with shadow behind them, and the fading plot of every moving contact. SW Radar paints a synthetic sea; SW Radar Over makes the clip the sea.',
  repo: 'https://github.com/stoatworks-labs/radar',

  blurb:
    'It is Radar’s own five GLSL passes, ported from the repository to WebGL2 and driven by a JavaScript port of the plugin’s CPU half — the antenna and its crossed-bin rule, the per-column crossing timing behind the phosphor’s decay, the clock, the contacts and the rain’s drift — which only a reader checks. There is no audio here, so the audio controls are absent. SW Radar reads no video; SW Radar Over turns a generated clip into the sea.',

  // The source is transparent outside the scope's circle; the backdrop shows it.
  showBackdrop: true,
  // RG32F and R32F render targets throughout, as in the plugin.
  needFloat: true,

  variants: {
    label: 'Plugin',
    default: initialVariant,
    options: [
      { id: 'source', name: 'SW Radar (source)', hint: 'RA01, FF_SOURCE: a synthetic coast, rain and moving contacts. Reads no clip.' },
      { id: 'over', name: 'SW Radar Over (effect)', hint: 'RA02, FF_EFFECT: the clip is the sea — its brightest channel over Threshold is what reflects.' },
    ],
  },

  // For the Over only. The geometry card's rings and grid read as a radar
  // picture at once; lights on black give point-like echoes.
  sources: ['grid', 'spot', 'scene', 'bars', 'alpha'],

  params: PARAMS,

  differences: [
    'The CPU half is a PORT, not the plugin’s code, and nothing checks it but a reader. The antenna’s position in double and the crossed-bin rule (Sweep.h), the per-column crossing timing that gives the Carry and Fade textures (Radar.cpp), the clock’s jump rule, the contacts and the rain’s drift (World.cpp), every control’s conversion (Controls.cpp), the ring spacing, the phosphor colours and the sinc half-power point are translated to JavaScript here. The repository’s ratest --sweep, --sweep-law, --persist and --clock check the C++ and have never heard of this page.',
    'The GPU half is not a port: the kernel, map, reflect, paint and composite passes are the plugin’s own GLSL, at the plugin’s own buffer sizes and formats (a 2048 × 1024 32-bit float phosphor), and demo/tools/check_shaders.py fails the repository’s verify script if a character of them drifts. The GLSL keeps the plugin’s bounded sine, cosine and bearingOf; the browser’s own trig is never used on the GPU.',
    'Nothing audio. The plugin declares an Audio FFT buffer that Resolume fills, with Audio Strobe (both plugins) and Audio Contacts (the source) over its onsets. A browser has no Resolume FFT, so all three are absent from this panel rather than present and dead. With no spectrum the plugin’s analyser never fires, so leaving them out changes nothing the page draws: no strobe spoke and no transient contact is ever made, here as in a Resolume layer with no audio.',
    'Contacts and Seed are FF_TYPE_INTEGER in the plugin (0–16 and 0–9999). The kit has no integer control, so they are dropdowns here: Contacts offers all seventeen values, Seed only 0–99. The About block is not on the panel.',
    'Switching the Plugin dropdown is a new instance, as it would be in Resolume: the phosphor, the antenna and the sea start again, and every control goes to that plugin’s constructor default, because the two plugins declare different controls and different defaults for five of the shared ones (Gain, STC, Clutter, Heading Line, Scope Size).',
    'The clock is the page’s: seconds from frame deltas, capped at a tenth of a second a frame, so on a slow machine the antenna turns slowly rather than jumping. The plugin’s vote on whether the host sends seconds or milliseconds is not ported, because this host’s unit is known. Restart is a clock jump, which the plugin takes as no time passing, so the antenna carries on from where it was; switching the Plugin dropdown is what starts it again. Paused, a moved look control redraws the frame without advancing the antenna.',
    'Over: the clip is the kit’s generated one, premultiplied, and the plugin reads the brightest channel times alpha — so over the transparency clip a half-transparent edge reflects as alpha squared, where Resolume’s own clips may arrive straight. GetMaxGLTexCoords is 1 here: the page’s textures are exactly the raster.',
    'The page needs 32-bit float render targets and bilinear float filtering (EXT_color_buffer_float, OES_texture_float_linear). The plugin keeps the phosphor in float32 because a paint carries the last one forward through a decay that can be within 1e-3 of 1; a browser without both is refused rather than handed a quietly wrong picture.',
    'Nothing here is measured. The plugin’s harness checks the arc’s width against Beamwidth, the streak against c τ / 2, the R⁻⁴ slope and STC, that every bearing bin is painted exactly once a rotation at any frame rate, the phosphor’s two-term decay to a few ulp, and the Over’s polar mapping of the clip, each with a negative control — and that harness, not this page, is the reason to believe the scope. The plugin itself has never been loaded into Resolume.',
  ],

  createRenderer: (gl, quad) => {
    renderer = createRenderer(gl, quad);
    return renderer;
  },
});

//---------------------------------------------------------------------------
// Which controls belong to which plugin, and whose defaults are in force.
//
// By inline style, not the `hidden` attribute: kit.css gives these elements a
// `display` of their own, which beats the attribute's user-agent rule.
//---------------------------------------------------------------------------
if (mounted) {
  const params = mounted.params;
  const embed = query.has('embed') && query.get('embed') !== '0';
  let shownVariant = mounted.state.variant;

  // Copy link carries the plugin, since the two have different defaults.
  const toQuery = params.toQuery.bind(params);
  params.toQuery = () => {
    const q = toQuery();
    if (mounted.state.variant === 'over') q.set('plugin', 'over');
    return q;
  };

  const show = (node, on) => { if (node) node.style.display = on ? '' : 'none'; };
  const rowOf = (name) => [...document.querySelectorAll('.prow')]
    .find((row) => row.querySelector('.prow__name')?.textContent === name);

  function showForVariant(variant) {
    for (const p of PARAMS) {
      if (p.only) show(rowOf(p.name), p.only === variant);
    }
    const effect = variant === 'over';
    for (const field of document.querySelectorAll('.transport__field')) {
      if (field.querySelector('.transport__label')?.textContent === 'Clip') show(field, effect);
    }
    show(document.querySelector('.transport__file'), effect);
  }

  document.addEventListener('demo:state', () => {
    const variant = mounted.state.variant;
    if (variant !== shownVariant) {
      shownVariant = variant;
      // A new instance: that constructor's defaults, for Defaults too.
      params.defaults = { ...DEFAULTS[variant] };
      params.reset();
    }
    if (!embed) showForVariant(variant);
  });

  if (!embed) {
    showForVariant(shownVariant);

    //-----------------------------------------------------------------------
    // A status line: reports, measures nothing.
    //-----------------------------------------------------------------------
    const stage = document.querySelector('.stage');
    if (stage) {
      const line = document.createElement('p');
      line.className = 'stage__status';
      line.id = 'radar-stats';
      line.setAttribute('aria-live', 'off');
      stage.append(line);
      setInterval(() => {
        const s = renderer?.stats;
        if (!s || s.rpm === undefined) return;
        const over = mounted.state.variant === 'over';
        line.textContent =
          `${s.rpm === 0 ? 'Antenna stopped' : `${sig(s.rpm)} rpm, ${(60 / s.rpm).toFixed(2)} s a rotation`}`
          + ` · ${Math.abs(s.rotations).toFixed(1)} rotations since this instance started`
          + ` · ${s.painted} of 2048 bearing bins painted in the last frame`
          + ` · rings every ${s.ringKm} km on a ${sig(s.rangeKm)} km scope`
          + (over ? '' : ` · ${s.targets} contact${s.targets === 1 ? '' : 's'}`);
      }, 250);
    }
  }
}
