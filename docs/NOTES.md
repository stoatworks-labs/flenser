# Notes — flenser

Repo-specific working notes. Cross-cutting fleet knowledge lives in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes); the *why* of the
code lives in `../AGENTS.md`.

---

## Where it came from

Started 2026-08-25 by copying the scaffolding out of `afterglow` (the newest
single-plugin repo, and the one carrying the fixed `cmake/InfoOFX.plist.in` and
the current `tools/verify.sh`) and the two-bundle shape out of `orrery` (the
`OBJECT` library, the `SourcePlugin.cpp`/`EffectPlugin.cpp` split, the release
workflow's matrix over two bundles).

Both classes of new-repo copy trap in
[new plugin repo copy traps](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_new_plugin_repo_copy_traps.md)
were checked before the first build: `InfoOFX.plist.in` is the parameterised
one, `vcpkg.json` exists, `Diag.cpp`'s `kAppName` and log env var are
`flenser`/`FLENSER_LOG_DIR`, and the FFGL plugin IDs are `FL01` and `FL02`,
which were free across the fleet.

## The decisions worth not re-litigating

**Closed form, not a fluid simulation.** The argument is at the top of
`source/Oil.h`. The short version: any frame renders on its own, the GPU and
the CPU can be *proved* to agree, and scrubbing works. `Simmer` is the one
bounded exception and is FFGL-only.

**The rim is drawn from the nearest single cell boundary, not from the merged
field.** This was built the other way round first and the picture looked
plausible — flat coloured shapes with clean outlines round the outside of the
pile. It just was not liquid. See the trap list in `AGENTS.md`.

**Churn is a fraction of the noise's wavelength.** Also learned by building it
the other way: at a high setting the plane folds and every cell becomes a nest
of contour lines. Dividing the amplitude by Grain removes that regime from the
control's range entirely.

**Gate is off at the top of its travel** rather than merely large, because the
corner of a 21:9 frame is 2.54 wheel units out. `fltest --null` depends on it.

## What has not been done

- ☠️ **Never loaded into Resolume, and never loaded into Resolve.** Everything
  in the "verified" list in `AGENTS.md` runs the plugin class directly, or runs
  the OpenFX bundle through `ofxprobe`. Neither is a host.
- **No Windows build has been run**, or even built — the release workflow's
  windows-latest job has never been dispatched.
- ~~**The Linux jobs have never run.**~~ Both ran on the v0.1.0 tag and both
  passed: the OFX bundle built in AlmaLinux 8 asks for nothing newer than
  GLIBC_2.28, and Rocky 8 `dlopen`s it and gets `OfxGetNumberOfPlugins -> 2`
  naming `com.stoatworks.flenser` and `com.stoatworks.flenserlamp`. Still a
  load and not a render.
- **The old note, for the record:** They were copied from `orrery`'s
  2026-08-25 commit, which is the shape the whole fleet now carries — see
  [linux ofx builds](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_linux_ofx_builds.md).
  Nothing here has been dispatched, so the glibc floor assertion and the Rocky 8
  `dlopen` are written and untested in this repo specifically.
- **The browser demo has never been watched running.** Its shaders are proved
  to be this repo's, its ported maths is proved to agree, and all four shaders
  compile and link as ES 3.00 in a real WebGL2 context — but the Browser pane
  used to check it reports `document.visibilityState === "hidden"`, so
  `requestAnimationFrame` never fires and `draw()` has never run. Same gap
  `afterglow` records.
- ~~**No release has been cut.**~~ **v0.1.0 shipped 2026-08-25**, with the full
  five-homes workup: seven artefacts (macOS universal bundle + dmg, Windows dll
  + NSIS installer, OpenFX for macOS, Windows and Linux), the macOS ones signed
  and notarised by the autosigner; the browser demo on its own custom domain; a
  project page, a suite entry and a published user guide; a project video
  (`QJ50vPvVlDc`) and an Instagram Reel; and a Burrow catalogue entry.
  Everything on the release checklist ran in its documented order — the tag,
  then the autosigner, then `check-notarised.py`, then `gen-downloads.py`
  scoped AND `--no-readme` unscoped, then `gen-catalog-data.py`.

## Registrations deliberately not made from here

These are edits to other repos, and they are listed rather than done so that
whoever cuts the first release does them together:

- **`stoatworks-backend`** — add `flenser` to `projects.json` (which is what
  feeds the About window's facts and `sync-about.py`), and to
  `resolume-demo/sync.sh`'s repo list so `demo/vendor/` stops being a hand
  copy. Note that `sync.sh`'s `projects=` path has not been updated for the
  2026-08-17 tree reorg — see
  [projects reorg broke fleet scripts](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_projects_reorg_broke_fleet_scripts.md).
- **`stoatworks-website`** — a project page, an entry on `/video-plugins`, and
  a hero in `scripts/shots.json`.
- ~~**Cloudflare**~~ — **done 2026-08-25.** `cf-run npx wrangler deploy` from
  the repo root attached `flenser-demo.stoatworks-labs.com` and Cloudflare
  created the proxied record itself; no dashboard step was needed. Verified by
  CONTENT rather than status code, and through `--resolve` against 1.1.1.1,
  because this Mac negative-caches a brand-new name for several minutes and
  otherwise reports `000` on a deploy that worked.
- **`~/.claude/launch.json`** already has a `flenser-demo` entry on port 8117,
  added while building this. That file is user config and is not in any repo.

## Ideas parked

- **A thickness profile inside a cell.** The dye coverage is currently a plate
  with a rim shoulder, which is right for oil squashed between two glasses. A
  spherical-cap profile would give a radial gradient and is probably wrong for
  the format, but is worth an experiment if the flat interiors ever look
  cheap.
- **Beat sync.** `downpour` and `orrery` take tempo from the FFGL host. Speed
  and Boil are the two controls that would take it. Not done because the OpenFX
  side has no tempo, and a control that exists in one build and not the other
  needs the same defence Simmer has.
- **Statically linking GLEW** is an open fleet question for `vectrix` only and
  does not apply here: nothing in this repo's OFX target touches GL.
