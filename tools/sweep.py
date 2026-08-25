#!/usr/bin/env python3
"""
No control is silently dead.

A GLSL uniform whose name does not match the C++ is ignored without a word:
glGetUniformLocation returns -1 and glUniform on -1 is a documented no-op. So a
slider can be wired to nothing while the plugin compiles, links, loads and
renders perfectly. Nothing in a build catches it and nothing in the picture
looks wrong -- the control just does not do anything, which is
indistinguishable from not having noticed what it is for.

This renders each parameter at several positions and checks the picture
actually changed.

The awkward part is that many controls are conditional: Dispersion does nothing
unless Refraction is up, Smear does nothing unless Simmer is, Boil does nothing
unless Churn is up AND enough frames pass for the field to move. A naive sweep
reports those as dead and buries the one real failure in five false ones, so
each such parameter carries the context it needs to mean anything. Those
contexts are the interesting content of this file -- if a parameter is added
without one and it turns out to be conditional, this will say so loudly.

    python3 tools/sweep.py [--binary build/fltest] [--source]
"""

import argparse
import hashlib
import pathlib
import subprocess
import sys
import tempfile

# What else has to be true for a parameter to have any effect at all. Without
# these the sweep reports a working control as dead.
#
# An entry beginning with "@" is a HARNESS setting, not a plugin parameter.
# That prefix is not decoration: this plugin has a parameter called "Cells" and
# the harness has a frame count, and without the marker "how many frames to
# render" and "how many cells are on the glass" would be the same string.
CONTEXT = {
    # The whole rim group needs a rim to draw on, and a lamp behind it to be
    # brighter or darker than.
    "Meniscus": ["Rim=0.5"],
    "Caustic": ["Rim=0.5"],
    "Rim": ["Meniscus=0.8", "Caustic=0.8"],
    # Dispersion is a DIFFERENTIAL on the refraction displacement, so with no
    # displacement there is nothing to differ.
    "Dispersion": ["Refraction=0.8"],
    # Refraction only shows where there is something to displace, which in the
    # generator is the lamp -- and a flat lamp displaced is a flat lamp. The
    # hot spot gives it a gradient to bend.
    "Refraction": ["Hotspot=0.8"],
    # Hue and its spread do nothing to a colourless wheel.
    "Hue": ["Saturation=0.9", "Density=0.9"],
    "Hue Spread": ["Saturation=0.9", "Density=0.9"],
    "Saturation": ["Density=0.9"],
    "Palette": ["Saturation=0.9", "Density=0.9", "Hue Spread=0.9"],
    # Gate Soft has nothing to soften with the gate off, and Gate is off at the
    # top of its own travel -- so it needs a position, not just a context.
    "Gate Soft": ["Gate=0.6"],
    # The noise field only moves if there is a field, and only measurably over
    # time. The harness drives a synthetic 60fps clock, so 90 frames is a
    # second and a half.
    "Boil": ["Churn=0.9", "@frames=90"],
    "Grain": ["Churn=0.9"],
    # Drift and Spin are displacements per second: with the wheel stopped they
    # displace nothing.
    "Drift": ["Speed=0.6", "@frames=90"],
    "Spin": ["Speed=0.6", "@frames=90"],
    "Speed": ["Drift=0.8", "@frames=90"],
    # Simmer is a feedback buffer: it needs frames to have gone into it, and
    # something moving for the carried-forward frame to differ from the live
    # one.
    "Simmer": ["Speed=0.6", "@frames=90"],
    "Smear": ["Simmer=0.8", "Speed=0.6", "Churn=0.9", "@frames=90"],
    # Variation and Scatter rearrange the wheel, which is invisible with one
    # cell on it.
    "Variation": ["Cells=0.8"],
    "Scatter": ["Cells=0.8"],
    # Mode and Mix are about the clip, so they mean nothing in the generator.
    # Swept in the effect build, which is what this file defaults to.
    "Mode": ["Density=0.9"],
    # The sweep's positions only ever reach preset 0 and preset 1. Preset 1 is
    # "Overhead Projector", which is deliberately NOT the plugin's defaults --
    # see the comment on it in Presets.h.
    "Preset": ["Density=0.5"],
}

# Positions to try. Three rather than two: a control that is a no-op at both
# ends but not in the middle is rare, but the bipolar ones -- Spin,
# Temperature -- are all NULL in the middle, so 0.0 and 1.0 are the two that
# have to differ and 0.5 is the one that must not be the only sample.
POSITIONS = ["0.0", "0.5", "1.0"]

# Parameters with no scalar float to sweep.
SKIP = set()


def render(binary, out, settings, frames, source):
    command = [binary, "--out", str(out), "--width", "320", "--height", "180",
               "--frames", str(frames)]
    command.append("--source" if source else "--effect")
    for setting in settings:
        command += ["--set", setting]

    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"render failed: {result.stderr.strip()}")
    return hashlib.sha256(out.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default="build/fltest")
    parser.add_argument("--source", action="store_true",
                        help="sweep the generator instead of the effect")
    arguments = parser.parse_args()

    binary = pathlib.Path(arguments.binary)
    if not binary.exists():
        print(f"no {binary} -- build with -DFLENSER_BUILD_TOOLS=ON first")
        return 2

    listing = subprocess.run([str(binary), "--list"], capture_output=True, text=True)
    if listing.returncode != 0:
        print("could not list parameters:", listing.stderr.strip())
        return 2

    names = []
    for line in listing.stdout.splitlines()[1:]:
        parts = line.split(None, 1)
        if len(parts) == 2:
            names.append(parts[1].rsplit(None, 1)[0].strip())

    # The About block is a text field and browser buttons, declared last. They
    # never touch a pixel, so sweeping them only buries a real dead control.
    if "About" in names:
        names = names[: names.index("About")]

    if not names:
        print("no parameters found")
        return 2

    # The generator has no clip, so the two controls that are about one are
    # genuinely inert there. Skipping them is not papering over anything --
    # they are swept in the default (effect) run.
    skip = set(SKIP)
    if arguments.source:
        skip |= {"Mode", "Mix"}

    dead = []
    with tempfile.TemporaryDirectory() as directory:
        out = pathlib.Path(directory) / "sweep.png"

        for name in names:
            if name in skip:
                continue
            context = list(CONTEXT.get(name, []))

            frames = 20
            for entry in list(context):
                if entry.startswith("@frames="):
                    frames = int(entry.split("=", 1)[1])
                    context.remove(entry)

            digests = set()
            for position in POSITIONS:
                try:
                    digests.add(render(binary, out, context + [f"{name}={position}"],
                                       frames, arguments.source))
                except RuntimeError as error:
                    print(f"  {name}: {error}")
                    dead.append(name)
                    break
            else:
                mark = "ok" if len(digests) > 1 else "DEAD"
                if len(digests) == 1:
                    dead.append(name)
                print(f"  {mark:4}  {name}")

    print()
    if dead:
        print(f"{len(dead)} parameter(s) changed nothing: {', '.join(dead)}")
        print("either the uniform name does not match the shader, or the sweep")
        print("needs a CONTEXT entry saying what else has to be true.")
        return 1

    swept = len([n for n in names if n not in skip])
    print(f"all {swept} swept parameters measurably change the picture")
    return 0


if __name__ == "__main__":
    sys.exit(main())
