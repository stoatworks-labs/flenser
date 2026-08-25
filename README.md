# Flenser

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. The model is not
> asserted but **measured**: the wheel's optics exist twice — once in GLSL for
> the GPU and once in C++ for the OpenFX build — and an offline harness
> compares the two across 1.6 million values over every control, finding no
> disagreement past 5e-4. Both builds are separately proved to leave a picture
> exactly as they found it when the wheel is clear: 0 of 230,400 bytes differ
> through the Resolume build, 0 of 921,600 through the OpenFX one. **It has not
> yet been run in Resolume or in Resolve** — see the "what is actually
> verified" section of [AGENTS.md](AGENTS.md), and check it in your own rig
> before trusting it in a show.

A liquid light show, as a plugin.

Oil, water, alcohol and dye between two watch glasses on the stage of an
overhead projector. Cells of oil press on each other and join with a fillet;
each one is a dye filter, so overlapping cells **multiply** — cyan over magenta
is blue, not white; and the meniscus at every edge is a lens that displaces
what is behind it, splits it by wavelength, and throws a bright caustic just
inside a dark rim. Behind all of it is a Fresnel condenser with a visible hot
spot, a colour temperature and a round gate.

It ships as **four plugins from one model**:

| | Resolume (FFGL) | Resolve, Nuke, Natron, Vegas (OpenFX) |
| --- | --- | --- |
| the wheel on its own | **Flenser** | **Flenser** |
| the clip as the lamp | **Flenser Lamp** | **Flenser Lamp** |

`Flenser Lamp` puts the incoming clip where the projector's lamp was: the
footage is what shines through the oil, gets dyed by it, and bends at every
meniscus.

> **Not affiliated with Optikinetics** or with any other maker of liquid-wheel
> projectors. The presets are named after the formats they imitate.

---

## What it looks like

Eight factory presets, in both builds, from one table:

| | |
| --- | --- |
| **Overhead Projector** | The DIY article: a clock glass on an OHP. A handful of big pools, barely moving, warm tungsten light, the round gate of the glass well inside the frame. |
| **Optikinetics** | The purpose-built unit: more cells, smaller, evenly packed, full frame, cold discharge lamp. |
| **Alcohol Burn** | The moment the alcohol goes in and the surface tension collapses. |
| **Ink in Water** | The subtractive primaries, thick and slow. The preset that shows what the dye stack is doing. |
| **Sodium** | The hot end of the palette only, dense, hot spot wide open. |
| **Clear Water** | No dye at all — every mark on the screen is refraction, a caustic or a meniscus line. |
| **Beading** | Merge at zero: separate hard-edged beads. A mask, a pixel-map driver, a title background. |
| **Slow Bloom** | Three enormous cells nearly filling the gate. Almost a colour wash. |

There is a browser demo that runs the plugin's own shaders, with nothing
uploaded: **https://flenser-demo.stoatworks-labs.com**

---

## Installing

[`docs/UNSIGNED.md`](docs/UNSIGNED.md) covers Gatekeeper, SmartScreen and the
firewall prompts, and how to verify a download.

**macOS.**

- Resolume: put `Flenser.bundle` and `Flenser Lamp.bundle` in
  `~/Documents/Resolume Arena/Extra Effects` (or the Avenue equivalent).
- Resolve and other OFX hosts: put `Flenser.ofx.bundle` in
  `/Library/OFX/Plugins`.

**Windows.** `Flenser.dll` and `Flenser Lamp.dll` into Resolume's extra effects
folder; `Flenser.ofx.bundle` into `C:\Program Files\Common Files\OFX\Plugins`.

**Linux.** The OpenFX build only — Resolume has no Linux version. Put
`Flenser.ofx.bundle` in `/usr/OFX/Plugins`. It is built against glibc 2.28 so
that it loads on Rocky 8, which is the Linux Resolve supports; anything newer
loads it too.

---

## The controls

Thirty-three of them, in seven groups. The long version is
[`docs/USER-GUIDE.md`](docs/USER-GUIDE.md); the short version is that the four
worth reaching for first are **Cells**, **Merge**, **Density** and **Churn**.

- **Wheel** — Cells, Size, Variation, Merge, Spread, Scatter, Seed.
  `Merge` is the one that decides whether the wheel is holding beads or pools.
- **Motion** — Speed, Drift, Spin, Churn, Grain, Boil.
  `Churn` is what stops the cells being circles, and is expressed as a fraction
  of the noise's own wavelength so that it deforms the oil rather than folding
  it.
- **Optics** — Density, Refraction, Dispersion, Meniscus, Caustic, Rim.
  `Density` is what makes the effect subtractive.
- **Colour** — Palette, Hue, Hue Spread, Saturation.
- **Lamp** — Lamp, Hotspot, Temperature, Gate, Gate Soft.
- **Simmer** — Simmer, Smear. **Resolume only**, and off by default; see below.
- **Output** — Mode, Mix.

`Mode` exists in the effect build only: **Project** puts the clip where the lamp
was, **Over** lights the oil itself and lays it on the clip, and **Colourise**
takes only the clip's brightness and lets the dye supply all the colour.

---

## The one difference between the two builds

**Simmer is in the Resolume build and not in the OpenFX one.**

Everything else in this plugin is a pure function of position and time — no
advection, no accumulator, no frame history. That is what lets any frame render
on its own, lets the GPU and the CPU be proved to agree, and makes scrubbing
show the wheel at that moment rather than restarting it.

Simmer is the deliberate exception: a feedback buffer, advected along the churn
field and carried forward under a ceiling. It is the one thing a closed-form
field genuinely cannot do — leave a trail behind a moving cell, because that
needs to know where the oil *was*.

OpenFX hosts render frames in any order, alone, on several threads at once.
Resolve rendering frame 500 before frame 4 is normal. A feedback buffer in that
host either serialises it or renders differently every time, and an effect that
does not match its own preview on the second export is worse than an effect
that is missing a control. So the OpenFX build does not have it and says so in
its own plugin description.

---

## Building

```bash
git clone --recursive https://github.com/stoatworks-labs/flenser
cd flenser
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

`CLAUDE.md` is the command reference. `AGENTS.md` is the *why*, including the
list of traps and an explicit account of what is verified and what is only
assumed — **read that before telling anybody this works in a host**, because at
the time of writing it has not been loaded into one.

Verify without a host:

```bash
tools/verify.sh
```

Which checks, among other things, that a clear wheel leaves the picture exactly
as it found it (0/255), and that the GLSL half of the model and the C++ half
still agree with each other across a million and a half comparisons.

---

## Licence

MIT. See [LICENSE](LICENSE) and [ATTRIBUTIONS.md](ATTRIBUTIONS.md).
