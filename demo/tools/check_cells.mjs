/**
 * Prove the demo's maths is the plugin's maths.
 *
 * `demo/plugin.js` copies the plugin's GLSL — `check_shaders.py` enforces
 * that. What it cannot copy is the C++: the control mappings in
 * `Controls.cpp` and the per-cell half of `Oil.cpp` are PORTED into
 * `demo/oil.js`, and a port is two files that happen to agree.
 *
 * This runs `fltest --cells`, which prints the plugin's own answers as JSON,
 * and compares them against the port's.
 *
 * Without it a drifted mapping or a drifted cell layout produces a page that
 * looks plausible, runs without an error, and does not behave like the
 * plugin — and on this plugin the gap is wide, because everything about a
 * cell comes out of `cellAt`. A mistake there is not a slider reading 0.47
 * instead of 0.5, it is the whole wheel being arranged differently.
 *
 *     node demo/tools/check_cells.mjs [path/to/fltest]
 *
 * Exit status is 0 when everything agrees, 1 otherwise, so it can go in
 * `tools/verify.sh`.
 */

import { execFileSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import {
  cellsFromParam, sizeFromParam, mergeFromParam, spreadFromParam,
  speedFromParam, driftFromParam, spinFromParam, churnFromParam,
  grainFromParam, boilFromParam, refractionFromParam, rimFromParam,
  causticFromParam, lampFromParam, temperatureFromParam, gateFromParam,
  cellAt,
} from '../oil.js';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(HERE, '..', '..');

/**
 * How far the port is allowed to disagree with the plugin, relatively.
 *
 * Not zero, and it cannot be. The plugin computes in float32 and JavaScript
 * has only doubles, so a chain of a dozen operations differs in the seventh
 * significant figure however carefully the port is written. Forcing every
 * intermediate through `Math.fround` would close that and would make the port
 * unreadable, which is a bad trade for a page whose job is to be legible.
 *
 * What this catches is a drifted CONSTANT — a golden angle of 2.4 against
 * 2.39996, an orbit frequency of 0.5 against 0.55, a palette offset with a
 * digit wrong. Those miss by percent.
 */
const TOLERANCE = 2e-5;

const MAPPINGS = {
  size: sizeFromParam,
  merge: mergeFromParam,
  spread: spreadFromParam,
  speed: speedFromParam,
  drift: driftFromParam,
  spin: spinFromParam,
  churn: churnFromParam,
  grain: grainFromParam,
  boil: boilFromParam,
  refraction: refractionFromParam,
  rim: rimFromParam,
  caustic: causticFromParam,
  lamp: lampFromParam,
  temperature: temperatureFromParam,
  gate: gateFromParam,
};

// The same four wheels `dumpCells()` resolves, described the way `oil.js`
// wants them. Kept here rather than exported from the harness because a
// checker that took its INPUTS from the thing it is checking would only prove
// the thing agrees with itself.
const DEFAULTS = {
  cells: 24, size: 0.18, variation: 0.35, merge: 0.06, spread: 0.9,
  scatter: 0.25, seed: 1, speed: 0.18, drift: 0.12, spin: 0.0,
  palette: 0, hue: 0.0, hueSpread: 0.7, saturation: 0.85, time: 0.0,
};

const WHEELS = {
  base: { ...DEFAULTS, cells: 17, seed: 7 },
  spun: { ...DEFAULTS, cells: 17, seed: 7, spin: 0.11, time: 3.7, scatter: 0.8, variation: 0.9 },
  inked: {
    ...DEFAULTS, cells: 17, seed: 7, palette: 1, hue: 0.3, hueSpread: 0.6,
    saturation: 0.75, time: 1.25, speed: 0.9,
  },
  spectrum: {
    ...DEFAULTS, cells: 41, seed: 4242, palette: 3, time: 9.5, speed: 0.4, drift: 0.3,
  },
};

function findHarness() {
  if (process.argv[2]) return process.argv[2];
  for (const candidate of ['build/fltest', 'build-universal/fltest']) {
    const full = path.join(ROOT, candidate);
    if (existsSync(full)) return full;
  }
  return null;
}

function differs(got, want) {
  const scale = Math.max(Math.abs(want), 1);
  return Math.abs(got - want) / scale > TOLERANCE;
}

function main() {
  const harness = findHarness();
  if (harness === null) {
    console.error('no build/fltest -- build with -DFLENSER_BUILD_TOOLS=ON first');
    return 2;
  }

  let plugin;
  try {
    plugin = JSON.parse(execFileSync(harness, ['--cells'], { encoding: 'utf8' }));
  } catch (error) {
    console.error(`could not run ${harness} --cells: ${error.message}`);
    return 2;
  }

  let checks = 0;
  let failures = 0;
  let worst = 0;
  let worstWhere = '';

  const compare = (where, got, want) => {
    checks += 1;
    const scale = Math.max(Math.abs(want), 1);
    const diff = Math.abs(got - want) / scale;
    if (diff > worst) {
      worst = diff;
      worstWhere = where;
    }
    if (differs(got, want)) {
      failures += 1;
      if (failures <= 8) {
        console.log(`  ${where}  demo ${got}  plugin ${want}`);
      }
    }
  };

  //---- the control mappings ------------------------------------------------
  for (const [name, fn] of Object.entries(MAPPINGS)) {
    const want = plugin.mappings[name];
    if (!want) {
      console.log(`  MISSING  the harness printed no mapping called "${name}"`);
      failures += 1;
      continue;
    }
    for (let i = 0; i <= 20; ++i) {
      compare(`${name}[${i}]`, fn(i / 20), want[i]);
    }
  }

  for (let i = 0; i <= 20; ++i) {
    compare(`cells[${i}]`, cellsFromParam(i / 20), plugin.cellsFromParam[i]);
  }

  //---- whole wheels --------------------------------------------------------
  for (const [name, wheel] of Object.entries(WHEELS)) {
    const want = plugin.wheels[name];
    if (!want) {
      console.log(`  MISSING  the harness printed no wheel called "${name}"`);
      failures += 1;
      continue;
    }
    if (want.length !== wheel.cells) {
      console.log(`  MISMATCH  wheel "${name}" has ${want.length} cells here and ${wheel.cells} there`);
      failures += 1;
      continue;
    }
    for (let i = 0; i < wheel.cells; ++i) {
      const got = cellAt(i, wheel);
      const labels = ['x', 'y', 'radius', 'dye.r', 'dye.g', 'dye.b'];
      for (let c = 0; c < 6; ++c) {
        compare(`${name}[${i}].${labels[c]}`, got[c], want[i][c]);
      }
    }
  }

  console.log(`cells: ${checks} comparisons, ${failures} past ${TOLERANCE}`);
  console.log(`cells: largest difference ${worst.toExponential(2)} (${worstWhere})`);

  if (failures) {
    console.log('\ndemo/oil.js is no longer computing what the plugin computes.');
    return 1;
  }
  return 0;
}

process.exit(main());
