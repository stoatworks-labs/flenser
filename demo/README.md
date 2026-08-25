# demo/ — the browser demo

Live at **https://flenser-demo.stoatworks-labs.com**, linked from the project
page and from the [video plugins
page](https://stoatworks-labs.com/video-plugins/).

**This is not the plugin.** It is the GLSL from
[`source/Shaders.cpp`](../source/Shaders.cpp), copied across unedited and run in
WebGL2 over clips generated in the page, with the parameters the plugin's
constructor declares. The page says so in a banner, and lists what it does not
reproduce at the foot.

All three passes are here in the order `ProcessOpenGL` runs them — the oil pass,
the optional simmer pass, the composite — including the ping-ponged feedback
buffer, so Simmer behaves as it does in Resolume rather than being flattened
out.

## Editing it

- `plugin.js` — this plugin's parameters and its shaders. **When a shader in
  `source/Shaders.cpp` changes, change it here too.** The two copies exist
  because the demo cannot include a C++ file; `tools/check_shaders.py` is what
  enforces that they agree, character for character, and it runs in
  `../tools/verify.sh`.
- `oil.js` — `Hash.h`, `Controls.cpp` and the per-cell half of `Oil.cpp`,
  ported. **No DOM and no WebGL in this file**, or `tools/check_cells.mjs`
  cannot import it under node.
- `vendor/` — the shared kit, vendored from
  `stoatworks-backend/resolume-demo/kit`. **Do not edit these.** Fix the master
  and re-run its sync; note that the sync's repo list does not include `flenser`
  yet, so these files were copied from `afterglow`'s copies by hand.

## The two checks, and why there are two

The demo is half a **copy** and half a **port**, and each half fails
differently.

- `tools/check_shaders.py` compares every shader literal in
  `source/Shaders.cpp` against the template literal in `plugin.js`, character
  for character. That half is a copy, so equality is the right test.
- `tools/check_cells.mjs` runs `fltest --cells` — a JSON dump of every control
  mapping and every resolved cell — and compares it against `oil.js`. That half
  is a port, so it is compared to a tolerance, and the tolerance is documented
  in the file.

The second one is the one worth defending. Everything about a cell comes out of
`cellAt`, so a mistake in the port is not a slider reading 0.47 instead of 0.5,
it is the whole wheel being arranged differently — and the page would still
look like a liquid light show while doing it.

## What the page cannot have

- **The clip is generated at exactly the canvas size**, so `MaxUV` is 1 and the
  host-texture-is-bigger-than-the-picture path is never exercised. That path is
  where a half-texel error would live, and here it cannot be seen.
- **The boil phase is `time × Boil`**, which is how the OpenFX build derives it.
  The Resolume build integrates the rate instead, so that nudging Boil live
  changes what happens next rather than rescaling the field's whole history.
  Scrub-ability was the trade, and this page is scrubbable.
- **No host clock, no host blend state, and no `plugMain`.** Everything that
  only a real host exercises is absent here by construction. See the
  "verified vs assumed" section of `../AGENTS.md`.

## Running it locally

Any static server will do — it is hand-written ES modules with no build step:

```bash
python3 -m http.server -d demo 8000
```
