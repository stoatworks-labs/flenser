/**
 * The plugin's maths, ported.
 *
 * This is `Hash.h`, `Controls.cpp` and the per-CELL half of `Oil.cpp`, in
 * JavaScript. It is a **port**, not a copy — the per-pixel half is not here at
 * all, because the page runs the plugin's own GLSL for that, which is the
 * whole point of copying the shader text rather than rewriting it.
 *
 * `demo/tools/check_cells.mjs` runs `fltest --cells` and compares its output
 * against this file, so a drifted mapping or a drifted cell layout is caught
 * rather than shipped. Until that check existed nothing tested the ported half
 * at all, and on THIS plugin the gap is wide: everything about a cell comes
 * out of `cellAt`, so a mistake in the port is not a slider reading 0.47
 * instead of 0.5, it is the whole wheel being arranged differently.
 *
 * **No DOM and no WebGL in this file.** The checker imports it directly under
 * node, where neither exists.
 */

//===========================================================================
// Hash.h
//===========================================================================

/**
 * Chris Wellons' `lowbias32`, exactly as `Hash.h` has it.
 *
 * `Math.imul` and `>>> 0` are load-bearing: a plain `*` on two 32-bit values
 * goes through a double and loses the low bits of the product, which is
 * precisely the half of the multiply this hash depends on. The result would
 * still look random and would agree with nothing.
 */
export function hash32(x) {
  let v = x >>> 0;
  v ^= v >>> 16;
  v = Math.imul(v, 0x7feb352d) >>> 0;
  v ^= v >>> 15;
  v = Math.imul(v, 0x846ca68b) >>> 0;
  v ^= v >>> 16;
  return v >>> 0;
}

export function hash2(a, b) {
  return hash32((a ^ hash32((b + 0x9e3779b9) >>> 0)) >>> 0);
}

export function hash3(a, b, c) {
  return hash32((a ^ hash2(b, c)) >>> 0);
}

/** The top 24 bits, to 0..1. Exact in a float32 and in a double alike. */
export function unit(h) {
  return (h >>> 8) * (1 / 16777216);
}

export function signed(h) {
  return unit(h) * 2 - 1;
}

//===========================================================================
// Controls.cpp
//===========================================================================

const clamp01 = (v) => Math.min(Math.max(v, 0), 1);
const lerp = (from, to, t) => from + (to - from) * clamp01(t);
const geometric = (from, to, t) => from * Math.pow(to / from, clamp01(t));

/** Geometric, but genuinely off at the bottom of the travel. */
function geometricFromZero(value, from, to) {
  if (value <= 0) return 0;
  return geometric(from, to, value);
}

/** 0.5 is the null, and each half maps to one direction. */
function bipolar(value, limit) {
  return (clamp01(value) * 2 - 1) * limit;
}

/** What `GateFromParam` returns when the gate is off. */
export const GATE_OFF = 1.0e6;

export const cellsFromParam = (v) => Math.round(geometric(1, 48, v));
export const sizeFromParam = (v) => geometric(0.03, 0.9, v);
export const variationFromParam = (v) => clamp01(v);
export const mergeFromParam = (v) => geometricFromZero(v, 0.004, 0.5);
export const spreadFromParam = (v) => lerp(0, 1.4, v);
export const scatterFromParam = (v) => clamp01(v);
export const seedFromParam = (v) => Math.round(lerp(1, 9999, v));

export const speedFromParam = (v) => lerp(0, 1.5, v);
export const driftFromParam = (v) => geometricFromZero(v, 0.002, 0.5);
export const spinFromParam = (v) => bipolar(v, 0.25);
export const churnFromParam = (v) => geometricFromZero(v, 0.004, 0.6);
export const grainFromParam = (v) => geometric(0.5, 12, v);
export const boilFromParam = (v) => lerp(0, 2, v);

export const densityFromParam = (v) => clamp01(v);
export const refractionFromParam = (v) => geometricFromZero(v, 0.0004, 0.12);
export const dispersionFromParam = (v) => clamp01(v);
export const meniscusFromParam = (v) => clamp01(v);
export const causticFromParam = (v) => lerp(0, 2, v);
export const rimFromParam = (v) => geometric(0.002, 0.15, v);

export const hueFromParam = (v) => clamp01(v);
export const hueSpreadFromParam = (v) => clamp01(v);
export const saturationFromParam = (v) => clamp01(v);

export const lampFromParam = (v) => lerp(0, 2, v);
export const hotspotFromParam = (v) => clamp01(v);
export const temperatureFromParam = (v) => bipolar(v, 1);
export const gateFromParam = (v) => (v >= 1 ? GATE_OFF : geometric(0.2, 2.6, v));
export const gateSoftFromParam = (v) => clamp01(v);

export const simmerFromParam = (v) => clamp01(v);
export const smearFromParam = (v) => lerp(0, 0.04, v);

//===========================================================================
// Oil.cpp -- the per-cell half
//===========================================================================

export const MAX_CELLS = 48;

const TAU = 6.28318530717958647692;
const GOLDEN_ANGLE = 2.39996322972865332;

export const PALETTE_NAMES = ['Aniline', 'Ink', 'Sodium', 'Spectrum', 'Duotone', 'Mono'];
export const LAMP_MODE_NAMES = ['Project', 'Over', 'Colourise', 'Matte'];

/** The one palette that is not a table: Spectrum is hashed anywhere on the wheel. */
const SPECTRUM = 3;

const PALETTES = [
  [0.0, 0.33, 0.58, 0.13], // Aniline
  [0.5, 0.83, 0.16],       // Ink
  [0.02, 0.06, 0.11],      // Sodium
  null,                    // Spectrum: continuous, hashed
  [0.0, 0.5],              // Duotone
  [0.0],                   // Mono
];

/** HSV to RGB, as `Oil.cpp` has it. */
export function hsvToRgb(h, s, v) {
  const hue = h - Math.floor(h);
  const sat = clamp01(s);

  const sector = hue * 6;
  const i = Math.floor(sector);
  const f = sector - i;

  const p = v * (1 - sat);
  const q = v * (1 - sat * f);
  const t = v * (1 - sat * (1 - f));

  switch (i % 6) {
    case 0: return [v, t, p];
    case 1: return [q, v, p];
    case 2: return [p, v, t];
    case 3: return [p, q, v];
    case 4: return [t, p, v];
    default: return [v, p, q];
  }
}

/**
 * Where cell `index` is this frame, how big it is, and what colour it dyes.
 * Returns `[x, y, radius, r, g, b]` — the same six numbers, in the same order,
 * that `fltest --cells` prints.
 */
/**
 * `Oil.cpp`'s `SetFreeRunningPhases`: the orbit and spin phases as a pure
 * function of the clock, `time * rate`.
 *
 * The Resolume build ANCHORS these instead, so that an operator moving Speed
 * or Spin changes what happens next rather than rescaling the whole history
 * and teleporting the wheel. This page is scrubbable, which is the same trade
 * the OpenFX build makes and for the same reason — the identical note already
 * applies to the boil phase below.
 */
export function setFreeRunningPhases(wheel) {
  wheel.orbitPhase = wheel.time * wheel.speed;
  wheel.spinPhase = wheel.time * wheel.spin;
  return wheel;
}

export function cellAt(index, wheel) {
  const count = Math.max(wheel.cells, 1);
  const seed = Math.max(wheel.seed, 0) >>> 0;
  const i = Math.max(index, 0) >>> 0;

  const hPos = hash2(i, seed);
  const hSize = hash3(i, seed, 0x51);
  const hOrbit = hash3(i, seed, 0xa7);
  const hDye = hash3(i, seed, 0x1d);

  // Two arrangements of the same disc, blended by Scatter. The spiral is
  // equal-AREA, so it stays even at every count.
  const k = (index + 0.5) / count;
  const spiralR = Math.sqrt(clamp01(k)) * wheel.spread;
  const spiralA = index * GOLDEN_ANGLE;

  const swarmR = Math.sqrt(unit(hPos)) * wheel.spread;
  const swarmA = unit(hOrbit) * TAU;

  const mix = (a, b, t) => a * (1 - t) + b * t;

  let px = mix(spiralR * Math.cos(spiralA), swarmR * Math.cos(swarmA), wheel.scatter);
  let py = mix(spiralR * Math.sin(spiralA), swarmR * Math.sin(swarmA), wheel.scatter);

  // A closed orbit per cell, on two incommensurate frequencies.
  const t = wheel.orbitPhase;
  const f1 = 0.55 + 0.9 * unit(hOrbit);
  const f2 = 0.5 + 1.1 * unit(hDye);
  const phase = unit(hPos);

  px += wheel.drift * Math.cos(TAU * (f1 * t + phase));
  py += wheel.drift * Math.sin(TAU * (f2 * t + phase * 1.37 + 0.21));

  const a = TAU * wheel.spinPhase;
  const ca = Math.cos(a);
  const sa = Math.sin(a);

  const x = px * ca - py * sa;
  const y = px * sa + py * ca;

  let radius = wheel.size * (1 + 0.9 * wheel.variation * signed(hSize));
  radius *= 1 + 0.12 * Math.sin(TAU * (0.31 * t + unit(hSize)));
  radius = Math.max(radius, 0.002);

  // Every cell gets a hashed dye strength as well as a hue: nobody charges a
  // wheel with a dozen identical concentrations.
  const hStrength = hash3(i, seed, 0xc3);
  const strength = 0.72 + 0.28 * unit(hStrength);

  // `?? PALETTES[0]` would be wrong here and quietly so: Spectrum's entry IS
  // null, and `null ?? fallback` takes the fallback -- so the continuous
  // palette would silently render as Aniline, in the demo only, with nothing
  // to see but four hues where there should be forty.
  const table = wheel.palette === SPECTRUM ? null : (PALETTES[wheel.palette] ?? PALETTES[0]);
  const offset = table === null
    ? unit(hDye)
    : table[hash32(hDye) % table.length];

  const dye = hsvToRgb(wheel.hue + wheel.hueSpread * offset, wheel.saturation, 1);

  // Towards CLEAR, by this cell's strength. A transmittance of 1 is clear
  // glass, so that is the correct direction to pull a weak dye in; towards
  // black would make a pale cell a dark one.
  for (let c = 0; c < 3; ++c) dye[c] = mix(1, dye[c], strength);

  return [x, y, radius, dye[0], dye[1], dye[2]];
}
