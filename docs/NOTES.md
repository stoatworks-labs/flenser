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
- **No Linux path.** The CMakeLists branches on APPLE and WIN32. `afterglow`
  has a Linux OpenFX-only job in its release workflow; that shape has not been
  copied here yet, and would need a `FLENSER_BUILD_FFGL=OFF` option that does
  not exist.
- **The browser demo has never been watched running.** Its shaders are proved
  to be this repo's, its ported maths is proved to agree, and all four shaders
  compile and link as ES 3.00 in a real WebGL2 context — but the Browser pane
  used to check it reports `document.visibilityState === "hidden"`, so
  `requestAnimationFrame` never fires and `draw()` has never run. Same gap
  `afterglow` records.
- **No release has been cut.** No tag, no signing, no notarisation, and
  `tools/verify.sh`'s OpenFX ad-hoc-sign check is the only part of the release
  path that has been exercised.

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
- **Cloudflare** — `wrangler.toml` names `flenser-demo.stoatworks-labs.com` as
  a custom domain, and that hostname does not exist yet. Deploying before the
  DNS record exists fails the whole deploy rather than deploying without it.
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
- **A Linux OpenFX job**, per the note above.
