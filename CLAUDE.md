# flenser

A liquid light show — oil, water, alcohol and dye between two watch glasses on
an overhead projector — as an FFGL source and effect for Resolume Arena/Avenue
and an OpenFX generator and filter for Resolve. C++/GLSL, CMake MODULE →
universal `.bundle` (macOS) + Windows `.dll`; the OpenFX half also builds for
Linux, where Resolve runs. Public MIT repo.

Read `AGENTS.md` before changing the field, the rim treatment or the churn.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build`
- Render a frame offline: `./build/fltest --out /tmp/frame.png`
- The generator instead of the effect: add `--source`
- List parameters and what they resolve to: `./build/fltest --list`
- Just the test card: `./build/fltest --card /tmp/card.png`
- Set a control: `--set "Cells=0.8" --set "Merge=0"` (repeatable, by display name)
- Apply a factory preset: `--preset "Overhead Projector"`
- Put real footage through the real shaders (for the project video):
  `ffmpeg … -f rawvideo -pix_fmt rgba - | ./build/fltest --pipe --width W --height H [--script cues.txt] | ffmpeg …`

## OpenFX build
- `source/ofx/FlenserOFX.cpp` → `build/Flenser.ofx.bundle` (target `FlenserOFX`,
  `-DBUILD_OFX=OFF` to skip) for Resolve/Vegas/Nuke/Natron. **Two plugins in one
  bundle**: `com.stoatworks.flenser` (generator) and `com.stoatworks.flenserlamp`
  (filter).
- Links Controls and Oil straight from source; only the per-pixel stage and the
  composite are mirrored on the CPU. Change that stage in `Oil.cpp` and
  `kOilLibrary` together, and run `--field`.
- **Simmer does not exist there**, and that is a decision, not a gap — see
  AGENTS.md.
- Smoke test: `../resolume-ofx-bridge/build/ofxprobe --dir build`
- Render: `… --render com.stoatworks.flenserlamp --size 640x360 --out /tmp/a.bmp`
- Prove the null invariant there too:
  `… --set density=0 --set refraction=0 --set meniscus=0 --set caustic=0 --set lamp=0.5 --set hotspot=0 --set temperature=0.5 --set gate=1`
  → "0 of N bytes differ".
- OFX SDK subset (BSD-3) vendored under `external/openfx`.
- Install for Resolve: copy the bundle into `/Library/OFX/Plugins` (macOS) or
  `/usr/OFX/Plugins` (Linux).
- **Linux**: `-DFLENSER_BUILD_FFGL=OFF` builds the OFX plugin alone, with no GL
  loader in the configure at all. Built in AlmaLinux 8 for glibc 2.28 (Rocky 8
  is what Resolve supports) and load-tested on Rocky 8. `Threads::Threads` is
  linked because `pthread_create` is still in libpthread at 2.28 and no static
  check catches its absence.

## Verify
- Everything: `tools/verify.sh`
- A clear wheel does not touch the clip: `./build/fltest --null`
- GLSL against C++: `./build/fltest --field`
- Presets survive every host, both bundles: `./build/fltest --presets`
- No dead controls: `python3 tools/sweep.py` and `python3 tools/sweep.py --source`
- The demo runs this repo's shaders: `python3 demo/tools/check_shaders.py`
- The demo runs this repo's maths: `node demo/tools/check_cells.mjs`
- Render cost: `./build/fltest --bench`
- Universal + exports:
  `lipo -archs "build-universal/Flenser.bundle/Contents/MacOS/Flenser"` and
  `nm -gU … | grep _plugMain`

## Notes
- **The dye stack MULTIPLIES.** `t *= mix(vec3(1), dye, cov*density)`, never
  `+=`. Cyan over magenta is blue. Making it additive removes the reason the
  plugin exists.
- **The rim comes from `dn`, not `d`.** `d` is the union's filleted surface;
  `dn` is the nearest single cell boundary. From `d` there is one meniscus round
  the whole pile and a dozen cells read as one flat shape.
- **Churn is a fraction of the noise WAVELENGTH.** `WarpPoint` divides by
  `grain`. Without that a strong churn folds the plane and every cell becomes a
  nest of contour lines.
- **Gate is OFF at 1.0**, not merely large — the corner of a 21:9 frame is 2.54
  wheel units out.
- **`CellAt` exists once**, and all four plugins call it. Only the per-pixel
  stage is written twice, in `Oil.cpp` and in `kOilLibrary`. Every mirrored line
  is marked `//= mirrored` in both. Change one, change both, run `--field`.
- Randomness is an integer PCG-style hash (`lowbias32`), never
  `fract(sin(x)*…)` — GLSL gives `sin` no accuracy requirement at all, so a
  mirrored effect cannot use one.
- `mix` in C++ must be written `x*(1-a)+y*a`, not `x+(y-x)*a`.
- The uniform ARRAYS go through `glUniform3fv` on `FindUniform`;
  `FFGLShader::Set` has no array overload and issues the wrong call silently.
- All host parameters are 0..1 and mapped in `Controls.cpp`. `SetParamInfo`
  clamps a standard default into 0..1 before `SetParamRange` can widen it.
- The generator sizes itself from `currentViewport`, which this class assigns in
  its own `InitGL` because it does not call the base.
- `FFGLScopedFBOBinding.h` is not in `FFGLSDK.h`; include it by hand.
- `ScopedFBOBinding` does not restore the viewport. Capture the host viewport at
  the top of `ProcessOpenGL` and restore it before the composite.
- macOS build must be universal (arm64 + x86_64). Verify with `lipo`, never the
  build log.
- The harness drives `SetTime` on a synthetic 60fps clock. Without it no time
  passes offline and the wheel is frozen.
- The harness has **one** row convention: the card is uploaded flipped and every
  readback is flipped, so everything outside `renderFrames` is top-down.
- Public repo. "Commit" = commit **and** push.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume), no bundle command. It covers the failures that actually happen: a
shader that will not compile (and the oil pass is assembled from three strings,
so the driver's line number refers to a file that does not exist), a buffer the
driver would not allocate, and the preset dropping back to Custom.

    ~/Library/Logs/flenser/flenser.YYYY-MM-DD.log

`FLENSER_LOG_DIR` overrides the directory.
