# AGENTS.md — Flenser

Onboarding for whoever (or whatever) picks this up next. `CLAUDE.md` is the
short command reference; this is the *why*. Read the "What is actually
verified" section before you tell anybody this works.

---

## What the plugin is

Two watch glasses clamped face to face with a few millilitres of oil, water,
alcohol and dye between them, on the stage of an overhead projector. The lamp
is under it, the mirror and lens are over it, and the operator squeezes the
glasses and lets the heat do the rest. That is a liquid light show, and it has
four separable behaviours, all of which this plugin models:

1. **Immiscible cells.** Oil in dyed water does not mix. It forms rounded cells
   that press on each other and join with a *fillet* rather than a crossing.
2. **Subtractive colour.** Each cell is a dye filter, so light through two of
   them is multiplied twice: cyan over magenta is **blue**, not white.
3. **The meniscus is a lens.** The edge of a cell is the only part of the wheel
   doing serious optics — it displaces what is behind it, splits it slightly by
   wavelength, and throws a bright caustic just inside a dark rim.
4. **The projector.** A Fresnel condenser with a visible hot spot, a lamp with
   a colour temperature, and a round gate.

It ships four ways from one model: as an FFGL source and effect (Resolume
Arena/Avenue) and as an OpenFX generator and filter (Resolve, Vegas, Nuke,
Natron).

---

## The three ideas the code is built on

### 1. The colour is subtractive, and that is most of the difference

`FieldAt` accumulates `t *= mix( vec3( 1 ), dye_i, coverage_i * density )` over
every cell. Not `+=`. Two overlapping dyes go *darker* than either, and the
crossings are a different colour from the cells that made them.

Nearly every other blob effect adds, and adding is what makes them look like
lava lamps rather than like this. If somebody "fixes" the dye stack into an
additive one because the overlaps look muddy, they have removed the reason the
plugin exists. The **Ink in Water** preset is there to make the difference
visible in one gesture.

### 2. Everything is a function of position and time

No advection, no accumulator, no frame history — the wheel at t=500 is computed
from t=500 and nothing else. That is not the most authentic way to boil oil,
and it buys three things a simulation cannot:

- **Any frame renders on its own.** OpenFX hosts render frames in any order,
  alone, on several threads at once; Resolve rendering frame 500 before frame 4
  is normal.
- **The GPU and the CPU can be proved to agree**, because there is a right
  answer that does not depend on how many frames have gone past. That is what
  `fltest --field` is.
- **Scrubbing works.** Dragging the playhead shows the wheel at that moment,
  not the wheel restarting.

`Simmer` is the deliberate exception; see below.

### 3. Per-cell on the CPU, per-pixel on both

`CellAt` in `Oil.cpp` takes an index and the wheel's settings and returns where
that cell is, how big it is and what colour its dye is. It never looks at a
pixel. At most forty-eight calls a frame — so **both builds call it** and the
GPU is handed the answers as two `vec3` uniform arrays. The arrangement exists
once and cannot drift.

Only the per-*pixel* stage is written twice, in `Oil.cpp` and in `kOilLibrary`
in `Shaders.cpp`. Every mirrored line carries a `//= mirrored` marker in both
files.

---

## The shape of the code

| File | What it is |
| --- | --- |
| `source/Hash.h` | An exact integer hash, and why it is not `fract(sin(x))`. |
| `source/Controls.{h,cpp}` | What a 0..1 slider position means, in physical units. One copy, used by both builds, the harness and the demo. |
| `source/Oil.{h,cpp}` | The model. `CellAt` (per cell, single copy) and the per-pixel stage (`WarpPoint`, `FieldAt`, `SynthLamp`, `GateAt`, `RimProfiles`, `BendAt`) which is mirrored in GLSL. |
| `source/Shaders.{h,cpp}` | The GLSL. Three passes; `kOilLibrary` is the mirrored per-pixel stage. |
| `source/PassBuffer.{h,cpp}` | An FBO that reallocates only when it has to and actually frees its colour texture. |
| `source/Flenser.{h,cpp}` | The FFGL plugin class: parameters, the passes, presets. Shared by both bundles. |
| `source/{Source,Effect}Plugin.cpp` | One `CFFGLPluginInfo` each. The one thing the two bundles must not share. |
| `source/Presets.h` | The factory presets, in host-facing 0..1. All four plugins read it. |
| `source/ofx/FlenserOFX.cpp` | The OpenFX plugins. Links Controls and Oil; mirrors the composite on the CPU. |
| `tools/fltest/main.cpp` | The offline harness: renders, checks, benchmarks, dumps. |
| `demo/` | The browser demo. Copies the GLSL, ports the C++, and both halves are checked. |

### The passes

1. **oil** — output size, into a buffer of ours. Warp the point, walk the
   cells, bend the lamp through the meniscus, stack the dyes, gate it.
   Everything expensive, once.
2. **simmer** — output size, ping-ponged. Optional; skipped entirely when
   Simmer is zero, which is the default.
3. **composite** — to the host's own framebuffer. Blends the simmer buffer in
   and mixes against the untouched clip.

---

## Traps

Roughly in the order they will bite.

### ☠️ The rim is drawn from `dn`, not from `d`

`FieldAt` returns two distances. `d` is the **union's** surface, filleted by
Merge; `dn` is the **nearest single cell boundary**, not filleted.

The rim treatment — the meniscus line, the caustic and the refraction — comes
from `dn`. Drawn from `d` it appears only on the outer silhouette of the wheel,
and a dozen cells piled together come out as one flat coloured shape with a
single meniscus round the outside of the pile. Every cell's edge is a real
oil-water boundary and every one of them is visible in a real projection,
including where one cell lies over another: they are stacked in the few tenths
of a millimetre between the glasses, not fused.

This was built the wrong way round first, and the picture it gave looked
plausible enough to ship — flat coloured shapes with clean outlines. It just
was not liquid.

### ☠️ Churn is a fraction of the noise's WAVELENGTH, not a distance

`WarpPoint` divides the amplitude by `grain` before it displaces anything.

Without that division, a displacement large against the noise's feature size
**folds the plane**: the field stops being a distance field, the rim band's
width varies wildly with position, and every cell comes out as a nest of
contour lines rather than as a shape. Tying the two together means Churn does
the same kind of thing at every Grain and cannot reach the folding regime at
all — which is why its default can be 0.90 rather than something timid.

### Merge maps to exactly zero, and `smin` divides by it

`MergeFromParam` has its geometric floor cut away at 0, because a control that
never quite stops merging is one that has to be fought rather than used. So
`smin` needs its `k <= 0` branch, in **both** mirrors, and that branch is the
control's null rather than defensive tidying.

### Gate is OFF at the top of its travel, not merely large

The corner of a 16:9 frame is 2.04 wheel units from the centre and a 21:9 one
is 2.54. A gate that only ever got large would still be cutting the corners off
a wide canvas at its maximum setting, and "the plugin rounds off my corners and
the control that should stop it is already at the top" is not something an
operator should have to work out. `GateFromParam` returns `kGateOff` at 1.0.

`fltest --null` asserts on that: with the gate anything less than off, the
picture does not come out byte-identical.

### `GateAt`'s softness floor is load-bearing

GLSL leaves `smoothstep` **undefined** when its two edges are equal, and Gate
Soft maps to exactly 0 at the bottom of its travel. Without the `max( soft,
1e-3 )` the hard-edged gate — the most useful setting the control has — is
whatever the driver felt like.

### The gate does not move with the churn

The churn moves the **oil**. The gate is a hole in the projector's casting and
the oil is on the glass above it, so `gateAt` takes the unwarped point. Warp
them together and a strong churn sloshes the aperture, which reads as the
camera moving rather than as the liquid moving.

### Never `fract( sin( x ) * 43758.5453 )`

GLSL gives `sin` **no accuracy requirement at all**. For a *mirrored* effect
that means the CPU and GPU builds cannot be made to agree even in principle,
and `--field` would have to be deleted rather than fixed. The noise here is an
integer PCG-style hash (`lowbias32`) on both sides, which is exact.

The same argument applies without the mirror: a composition built on the show
laptop and opened on the rack machine has to arrange its cells the same way.

### `mix` is `x*(1-a) + y*a`, and it has to be written that way in C++

`x + (y-x)*a` is algebraically identical and is a different sequence of
floating-point operations. `Oil.cpp` has a `mixf` that spells GLSL's form out,
and it is compared against a shader's `mix` to five decimal places.

### A float `mod` is not safe for an index

GLSL defines `mod` as `x - y*floor(x/y)`; on an exact multiple the division can
round a hair below the integer, `floor` drops a whole step, and the result is
`y` instead of `0` — **on the GPU only**, so a CPU mirror will not reproduce
it. Nothing here uses one; keep it that way.

### `FFGLShader::Set` has no array or integer-vector overload

The overloads are `float`, `vec2`, `vec3`, `vec4` and `int` — nothing else.
`CellPos` and `CellDye` go through `glUniform3fv` on `FindUniform(...)`
directly. Asking `Set` for one issues the wrong `glUniform` against the wrong
type, which is a `GL_INVALID_OPERATION` that leaves the uniform at zero with
nothing anywhere the plugin can see — and zero here is every cell at the origin
at zero radius, i.e. a plugin that renders an empty wheel and looks switched
off.

The same trap has a different shape in the demo: the kit's `set()` issues
`glUniform1f`, and the array uniforms need `setArray(name, values, 3)` while
the samplers and ints need `setSampler`/`setInt`. It was written wrong there
first and the console said `Uniform size does not match uniform method`.

### A GLSL uniform name that does not match the C++ is silently ignored

`glGetUniformLocation` returns −1 and `glUniform(-1)` is a documented no-op, so
a control can be stone dead while everything compiles, links, loads and
renders. `tools/sweep.py` is the only thing that catches it, and it runs both
bundles.

### A TEXT parameter without `SetTextParameter` kills the whole plugin

`instantiateGL` pushes every declared default back through the setters and
deletes the instance the moment one returns `FF_FAIL` — which is exactly what
`CFFGLPlugin::SetTextParameter` does. The About block is display-only text, so
there is nothing to store, but it has to say so *successfully*. Invisible in
every in-repo harness, because they call the plugin class directly.

### The generator has no input texture to take a size from

It uses `currentViewport`, which the SDK's base `InitGL` sets — and this class
overrides `InitGL` without calling the base, so it assigns it itself. Miss that
and the generator sizes itself from zeros and returns `FF_FAIL` every frame,
which in Resolume is a source that draws nothing.

### A ranged STANDARD parameter cannot have a ranged default

`SetParamInfo` clamps a standard default into 0..1 *before* returning, and
`SetParamRange` can only be called afterwards. So every numeric parameter here
is a plain 0..1 float and the conversions live in `Controls.cpp`.

### Option lists are sorted; option VALUES are not

`SetParamElementInfo` takes an element's display slot and its stored value as
different arguments, and the spec is explicit that picking an option stores the
option's *value*. So the lists can be re-sorted for whoever has to read them
without a saved composition, a preset or the harness changing meaning.

### Presets: the host owns parameter state and does not consume value events

Reported against vertigo as its issue #2 and copied into seven plugins before
anybody noticed. Resolume does not act on the `FF_EVENT_FLAG_VALUE` events a
plugin raises; it carries on pushing the values it still believes in, which are
the ones from *before* the preset. So "a covered parameter changed, therefore
the operator has taken over" fires on the host's own echo, immediately, and the
dropdown snaps back to Custom.

The fix here keeps `hostValues[]` — what the host last *sent* — separately from
what the plugin renders with, and does not write the host's restatement into
`params[]` at all.

Two things in that mechanism will bite again:

- **`seedHostValues()` must run BEFORE `applyPreset` can.** Seeding lazily
  inside the guard would record the preset's own values as the host's opening
  position, and the host's very next restatement would look like an edit.
- **The two tolerances are different numbers on purpose.** The host-restatement
  test uses 1e-3, a quantisation allowance; the "did a covered parameter move?"
  test uses 1e-4. A value that *matches* the preset must be ignored, not
  written — writing a host's rounded copy of our own value trips the tighter
  test.

`fltest --presets` drives three hosts (honours the events / ignores them /
honours-but-quantises) across every preset in **both** bundles, with no GL.

### `ScopedFBOBinding` restores the framebuffer and not the viewport

SDK b1afaf9. Every pass's `ResizeViewPort()` leaks into the next one, and the
composite — which draws to the host's own framebuffer and so has no buffer to
size itself from — inherits whatever the last pass left. Capture `GL_VIEWPORT`
at the top, restore it before the composite.

### Every `ffglex::Scoped*` binding CLEARS to 0 on scope exit

It does not restore. `FFGLFBO::Initialise` sizes its new colour texture under
one of those, so **allocating a buffer unbinds the input texture from the
active unit**. Every `Ensure()` in `ProcessOpenGL` happens before anything
binds a texture, and it has to stay that way. The symptom is the dangerous
part: correct on every frame except the one that allocates.

### `FFGLFBO::Release()` leaks the colour texture

It deletes the framebuffer and the depth renderbuffer, then tests
`depthBufferID` a second time where it plainly meant `colorTextureID`.
`PassBuffer::Destroy()` deletes it first.

### `layout` is a GLSL keyword

So are `flat`, `active`, `filter`, `input`, `output`, `sample`, `common`. A
shader that fails to compile surfaces only at runtime, as "the effect does
nothing" — and the oil pass is **assembled from three strings**, so the line
number a driver reports is in a file that does not exist. That is what `Diag`
is for.

### `FFGLScopedFBOBinding.h` is not in the umbrella header

`FFGLSDK.h` includes every other scoped binding and omits that one.

### macOS must build universal, and the log will lie about it

CMake latches `CMAKE_OSX_ARCHITECTURES` when the first target is created, so
setting it late is silently ignored and the build still logs a success. Only
`lipo` is honest.

### `cmake/InfoOFX.plist.in` is the file that catches new repos

The version this repo would have been copied from spelled the *previous*
plugin's name into `CFBundleExecutable`. Nothing catches that locally: the
bundle assembles, the binary is correct, `nm` finds `_OfxGetPlugin`, and
`ofxprobe` renders through it. It fails at release time, in `codesign`, with a
message about a "subcomponent" that never mentions the plist. This repo's copy
is parameterised on `@PROJECT_NAME@`, and `tools/verify.sh` runs the release
step locally.

### ☠️ `pthread_create` is in libpthread until glibc 2.34

The OFX Support library's multi-thread suite calls it, and Rocky 8 — the Linux
Resolve supports — is glibc 2.28. So a build on any modern distro links and
runs while the same source refuses to `dlopen` on the one distro that matters,
with `undefined symbol: pthread_create`.

**It passes every static check**: it compiles, links, exports `OfxGetPlugin`
and passes a glibc-version assertion, because a version check reads the symbols
a binary *asks for* and says nothing about whether anything provides them. Only
running it finds it. Hence `find_package(Threads REQUIRED)` and
`Threads::Threads` on the OFX target, and hence the `linux-load` job. Four
plugins in the fleet failed on exactly this.

### `vcpkg.json` is invisible from the CMakeLists

GLEW arrives through the vcpkg manifest, and the CMakeLists never mentions it —
so every local build and every macOS CI job passes while the Windows job fails
at *configure*.

### The harness has ONE row convention, and it cost an afternoon

GL treats a texture's first row as the bottom one and reads a framebuffer back
the same way; the test card, a PNG and a raw frame from ffmpeg are all
top-down. `renderFrames` therefore uploads the card FLIPPED and flips every
readback, so everything outside that function is top-down.

Written the other way round — upload as-is, flip only where a PNG is written —
`--null` compared the top of the output against the bottom of the card and
reported **255/255 on a plugin that was behaving perfectly**, and the two PNGs
`--out` and `--card` write came out of one run the opposite way up.

---

## The two real differences between the builds

### Simmer

The FFGL build has it; the OpenFX build does not, and does not claim to.

It is a feedback buffer — the previous frame, advected along the churn field
and carried forward. It is the one thing a closed-form field genuinely cannot
do: leave a trail behind a moving cell, because that needs to know where the
oil *was*.

OpenFX renders frames in any order, alone, on several threads. A feedback
buffer there either serialises the host or renders differently every time, and
an effect that does not match its own preview on the second export is worse
than an effect that is missing a control.

Two properties keep it honest where it does exist:

- **At Simmer 0 the picture is `oil` exactly** — `mix( oil, max( oil, feed ),
  Simmer )`, so the null is the same numbers, not nearly. That is what lets the
  pass be skipped without the picture jumping on the frame it is switched on.
- **It cannot run away.** A `max` against the live frame with a persistence
  strictly below 1 means nothing in the buffer can ever be brighter than the
  brightest oil that has been through it. A feedback loop with a gain of 1.02
  is a white screen four hundred frames later, in the middle of a show, and no
  amount of care with the default value prevents that; a ceiling does.

### The boil phase

The FFGL build **integrates** the Boil rate, so that nudging the control live
changes what happens next instead of rescaling the whole history and jumping
the noise field somewhere else — worst at the moment somebody is watching the
control they are moving.

The OpenFX build uses `time * boil`, because there is no previous frame to have
integrated from and a deterministic frame matters more in a host that renders
them out of order. The browser demo does the same, and says so.

The cell orbits are **not** integrated in either build: an orbit is a closed
loop, and rescaling its phase moves a cell along a path it was going to travel
anyway. The distinction is worth keeping straight.

---

## The browser demo

`demo/` is a WebGL2 page that runs the plugin's own GLSL over clips generated
in the page. It is not the plugin, and it says so in a banner.

It **copies** the shader text and **ports** the C++, and both halves are
checked:

- `demo/tools/check_shaders.py` compares every shader literal in
  `source/Shaders.cpp` against the template literal in `demo/plugin.js`,
  character for character.
- `demo/tools/check_cells.mjs` runs `fltest --cells` — a JSON dump of every
  control mapping and every resolved cell — and compares it against
  `demo/oil.js`.

The second is the one worth keeping. Everything about a cell comes out of
`cellAt`, so a mistake in the port is not a slider reading 0.47 instead of 0.5,
it is the whole wheel being arranged differently — and the page would still
look like a liquid light show while doing it.

The pure maths therefore lives in `demo/oil.js`, which touches no DOM and no
WebGL. Keep it that way, or the checker cannot import it.

**One real bug the port had, and it is the shape to watch for:**
`PALETTES[wheel.palette] ?? PALETTES[0]` looks like a safe default and is
wrong — Spectrum's entry *is* `null`, and `null ?? fallback` takes the
fallback, so the continuous palette silently rendered as Aniline. Four hues
where there should have been forty, in the demo only, with nothing to see.

---

## What is actually verified, and what is assumed

### Verified by measurement, on an M4 Max running macOS 26.4

- **The GLSL oil library against the C++ one**: 1,625,010 comparisons across
  twenty parameter cases, two spans and every mirrored function; **0
  disagreements past 5e-4**, largest observed difference 2.5e-4, 30
  comparisons skipped for sitting on a deliberate discontinuity of the hard
  `min`.
- **A clear wheel does not touch the clip.** The FFGL build: **0 of 230,400
  bytes differ from the input**. The OpenFX build, through `ofxprobe`: **0 of
  921,600 bytes differ**. Two independent implementations of the whole clip
  path, both byte-exact.
- **No dead controls**: all 33 parameters measurably change the picture in the
  effect build, and all 31 that apply in the generator.
- **Factory presets**: all 8, against all 3 host behaviours, in both bundles —
  48 cases.
- **The demo's shaders** are this repo's, character for character (7 literals).
  **The demo's maths** agrees with the plugin's across 888 comparisons, largest
  difference 2.6e-6.
- **The demo's four shaders compile and link** as ES 3.00 through the kit's
  `port()`, in a real WebGL2 context, and the oil program resolves both cell
  uniform arrays.
- **The OpenFX bundle** loads and renders through `ofxprobe`, exports
  `_OfxGetPlugin`, names its own binary in its plist, and ad-hoc signs. Both
  contexts are recognised: the generator is correctly skipped by a
  Filter-context probe, and the filter renders.
- **The macOS bundles** are universal (`x86_64 arm64`) and export `plugMain`.
- **Render cost**: 1.2 ms/frame at 1080p and 3.3 ms at 4K with the defaults;
  1.4 ms and 4.6 ms with 48 cells and Simmer on. Measured with a static test
  card — the first version of `--bench` rebuilt the card every frame and
  reported 12.6 ms at 1080p, which was the harness's own trigonometry and not
  the plugin.

### Assumed, not measured

- ☠️ **It has never been loaded into Resolume.** Everything above runs the
  plugin class directly in a headless GL context. The things that only a real
  host exercises are: `plugMain` and `instantiateGL` (the `SetTextParameter`
  trap lives there), whether Resolume honours the parameter groups, and what
  the host's clock and blend state actually look like on the way in.
- ☠️ **It has never been loaded into DaVinci Resolve.** `ofxprobe` is a
  faithful harness but it is not Resolve, and in particular it is not a host
  that renders frames out of order across several threads — which is the
  condition the OpenFX build's whole design assumes.
- ☠️ **Nothing built for Windows has ever been RUN.** It builds: the v0.1.0
  release job produced both DLLs, the OpenFX bundle and an NSIS installer, so
  the GLEW-from-vcpkg path configures and links. Nothing has loaded one into
  Resolume on Windows.
- **The Linux OpenFX build LOADS and has never rendered.** On the v0.1.0 tag
  the bundle built in AlmaLinux 8 for glibc 2.28, the assertion confirmed it
  asks for nothing newer, and Rocky 8 `dlopen`ed it and got
  `OfxGetNumberOfPlugins -> 2` back naming both plugin identifiers — which runs
  the Support library's static initialisers and constructs the factories. That
  proves the binary is loadable on the distro Blackmagic ships for. It does not
  render a frame, and Resolve for Linux is x86_64-only and needs a GPU, so that
  cannot be closed from the Apple Silicon machine this was written on.
- **There is no Linux FFGL build and there will not be**: Resolume has no Linux
  version and the FFGL SDK has no Linux path. `FLENSER_BUILD_FFGL=OFF` is what
  lets the OpenFX half configure without a GL loader anywhere.
- **The browser demo has never been watched running.** Its shaders are proved
  to be this repo's, its maths is proved to agree, and every program compiles
  and links in a real WebGL2 context — but the Browser pane used to check it
  reports `document.visibilityState === "hidden"`, so `requestAnimationFrame`
  never fires, the canvas is never sized and `draw()` has never run. The render
  loop itself is a straight port of the C++ loop that *is* verified, and that
  is the whole of the argument for it.
- **The render cost has been measured only at the sizes `--bench` uses**, on
  one GPU, and only for the FFGL build. The OpenFX build's cost is a CPU walk
  over every cell at every pixel and has never been measured at all — it is the
  number most likely to be a problem and the one there is no figure for.
- **Nothing has been through a real show.**

---

## Sibling projects

- **`resolume-ofx-bridge`** — `build/ofxprobe` is what loads and renders the
  OpenFX build here. `--edit name=value` delivers a real user edit and fires
  `instanceChanged`, which is the only way preset logic runs headless.
- **`afterglow`, `orrery`, `downpour`, `vertigo`, `tinsel`, `porthole`,
  `old-cathode`** — the same scaffolding, the same About block, the same preset
  mechanism and the same release workflow. `orrery` and `downpour` are the two
  other repos that ship a source and an effect from one class; a fix to that
  shape belongs in all three.
- **`stoatworks-backend`** — the masters for `source/StoatworksAbout*.h`,
  `demo/vendor/`, `scripts/release-lib.sh` and `ATTRIBUTIONS.md`. Edit them
  there and re-run the sync; the copies here are build inputs.

## What is still to do

See `docs/NOTES.md` for the list, including the registrations in
`stoatworks-backend` and on the website that have deliberately not been made
from here.
