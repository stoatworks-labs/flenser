#!/usr/bin/env bash
#
# Everything that can be checked without a host, in one go.
#
#   tools/verify.sh
#
# Eight things get checked, and they fail in different ways:
#
#   --continuity  that moving Speed or Spin changes what happens NEXT and
#              does not move what is already drawn. Exact, not a threshold:
#              the frame the control moves on must be bit-identical to the
#              frame that would have been drawn untouched, and the frame
#              after it must differ. Shipped broken once (#1).
#
#   --null     that a clear wheel does not touch the clip. This is the one
#              that matters. With no dye, no refraction and no rim the effect
#              build is a piece of clear glass, and the picture has to come
#              out exactly as it went in -- 0/255, not "close". A half-texel
#              error in the clip fetch, a MaxUV applied twice, an intermediate
#              buffer at the wrong precision and a composite that lifts black
#              all sit on that path, and all of them look like "it's a bit
#              soft" rather than like a bug.
#   --field    the GLSL oil library against the C++ one, over every control,
#              two spans and every mirrored function. The two copies exist
#              because the GPU cannot call C++ and the OpenFX build cannot
#              call GLSL; nothing else notices when they drift.
#   --presets  that every factory preset survives every host behaviour, in
#              both bundles. No GL: this is the parameter plumbing, and it is
#              the half an external user actually got stuck on (vertigo
#              issue #2).
#   sweep.py   that no control is silently dead, in BOTH builds. A GLSL
#              uniform whose name does not match the C++ is ignored without a
#              word, so this is the only thing standing between a typo and a
#              shipped slider that does nothing.
#   the demo   TWO checks, because the demo copies half of this repo and ports
#              the other half. check_shaders.py proves demo/plugin.js still
#              holds this repo's shader text character for character;
#              check_cells.mjs proves demo/oil.js still computes what
#              Controls.cpp and Oil.cpp compute. Without the second one a
#              drifted mapping or a drifted cell layout produces a page that
#              looks plausible, runs without an error and does not behave like
#              the plugin.
#   the OFX
#   bundle     that Info.plist names the binary that is really there, and that
#              the bundle ad-hoc signs. Both are release-time failures with no
#              local symptom at all -- see below.
#   the binary that the macOS build is universal and still exports plugMain.
#              Checked with lipo and nm rather than by reading the build log,
#              because an arm64-only build logs as a success.
#
#   --bench    the render cost. Not pass/fail -- there is no threshold worth
#              asserting on somebody else's GPU -- but a verify run leaves a
#              timing on the record, which is what turns "it feels slower"
#              into a comparison.
#
# The field check reports a tolerance rather than demanding equality, and that
# is not a fudge: the GLSL specification allows three units in the last place
# for exp and gives sin no accuracy requirement at all, so the two
# implementations cannot agree bit for bit and a test that insisted would fail
# on every driver. See kFieldTolerance in tools/fltest/main.cpp.
#
# ---------------------------------------------------------------------------
# Why the OpenFX bundle is checked here rather than only in the release job
#
# cmake/InfoOFX.plist.in is one of the files a new plugin repo starts life by
# copying, and the version downpour had spelled the PREVIOUS plugin's name out
# in CFBundleExecutable. NOTHING caught it: the bundle assembles, the binary is
# correct, nm finds _OfxGetPlugin, and ofxprobe loads it and renders a correct
# frame. It fails at RELEASE time, in codesign, with a message about a
# "subcomponent" that never mentions the plist -- which is after the tag.
#
# So the check is the release step itself, run here where it costs a second.
# The general lesson, and it is the reason this file exists at all: anything
# the release job does that can be done locally should be done locally.
# ---------------------------------------------------------------------------
set -uo pipefail

cd "$(dirname "$0")/.."

BUILD=build

if [[ ! -x $BUILD/fltest ]]; then
	echo "$BUILD/fltest not found. Run:"
	echo "  cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build"
	exit 1
fi

failures=()

echo "== null: a clear wheel does not touch the clip"
if ./$BUILD/fltest --null --width 320 --height 180; then
	:
else
	failures+=("null")
fi

echo
echo "== continuity: moving a rate control does not move the picture"
if ./$BUILD/fltest --continuity --width 320 --height 180; then
	:
else
	failures+=("continuity")
fi

echo
echo "== matte: the cutout mode, identical in both builds and premultiplied"
if ./$BUILD/fltest --matte --width 320 --height 180; then
	:
else
	failures+=("matte")
fi

echo
echo "== field: GLSL against C++"
if ./$BUILD/fltest --field | tail -2; then
	:
else
	failures+=("field")
fi

echo
echo "== presets: every factory preset survives every host behaviour"
if ./$BUILD/fltest --presets | tail -1; then
	:
else
	failures+=("presets")
fi

echo
echo "== sweep: no control silently dead"
for variant in effect source; do
	flag=""
	[[ $variant == source ]] && flag="--source"
	if python3 tools/sweep.py $flag > "/tmp/flenser-sweep-$variant.txt" 2>&1; then
		echo "   $variant: $(tail -1 "/tmp/flenser-sweep-$variant.txt")"
	else
		echo "   *** dead controls in the $variant build, see /tmp/flenser-sweep-$variant.txt"
		tail -4 "/tmp/flenser-sweep-$variant.txt"
		failures+=("sweep ($variant)")
	fi
done

echo
echo "== demo: the browser demo's GLSL against this repo's"
if python3 demo/tools/check_shaders.py | tail -1; then
	:
else
	echo "   *** demo/plugin.js is no longer running the plugin's shader"
	failures+=("demo shaders")
fi

echo
echo "== demo: the browser demo's ported maths against this repo's"
if command -v node >/dev/null 2>&1; then
	if node demo/tools/check_cells.mjs | tail -2; then
		:
	else
		echo "   *** demo/oil.js is no longer running the plugin's maths"
		failures+=("demo maths")
	fi
else
	echo "   skipped: no node"
fi

echo
echo "== OpenFX: the bundle names its own binary, and signs"
if [[ -d "$BUILD/Flenser.ofx.bundle" ]]; then
	plist="$BUILD/Flenser.ofx.bundle/Contents/Info.plist"
	named=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$plist" 2>/dev/null)
	if [[ ! -f "$BUILD/Flenser.ofx.bundle/Contents/MacOS/$named" ]]; then
		echo "   *** Info.plist names \"$named\", which is not in Contents/MacOS"
		failures+=("ofx plist")
	else
		# On a COPY, so a verify run never leaves a signature on the build tree
		# that the release job did not put there.
		scratch="${TMPDIR:-/tmp}/flenser-signcheck.ofx.bundle"
		rm -rf "$scratch"
		cp -R "$BUILD/Flenser.ofx.bundle" "$scratch"
		if codesign --force --sign - --timestamp=none "$scratch" >/dev/null 2>&1; then
			echo "   CFBundleExecutable is $named, and the bundle ad-hoc signs"
		else
			echo "   *** the OpenFX bundle will not codesign"
			codesign --force --sign - --timestamp=none "$scratch" 2>&1 | sed 's/^/       /'
			failures+=("ofx codesign")
		fi
		rm -rf "$scratch"
	fi

	# Captured, then matched from a herestring -- never `nm ... | grep -q`.
	# Under `set -o pipefail` a `grep -q` that finds its match exits
	# immediately, the writer upstream takes SIGPIPE, and the PIPELINE reports
	# failure even though the symbol is there. It is output-size dependent, so
	# it fires on the bigger binary first and looks intermittent.
	symbols=$( nm -gU "$BUILD/Flenser.ofx.bundle/Contents/MacOS/$named" 2>/dev/null || true )
	if grep -q '_OfxGetPlugin' <<<"$symbols"; then
		echo "   exports _OfxGetPlugin"
	else
		failures+=("no _OfxGetPlugin export")
	fi
else
	echo "   skipped: no OpenFX bundle. Configure without -DBUILD_OFX=OFF."
fi

echo
echo "== bench: the render cost, for the record"
./$BUILD/fltest --bench --frames 20 2>&1 | sed -n '4,8p'

echo
echo "== binary: universal, and exports plugMain"
for bundle_name in "Flenser" "Flenser Lamp"; do
	bundle="build-universal/${bundle_name}.bundle/Contents/MacOS/${bundle_name}"
	if [[ -f "$bundle" ]]; then
		architectures="$(lipo -archs "$bundle" 2>/dev/null)"
		echo "   ${bundle_name}: $architectures"
		[[ "$architectures" == *arm64* && "$architectures" == *x86_64* ]] \
			|| failures+=("${bundle_name} not universal: $architectures")

		symbols=$( nm -gU "$bundle" 2>/dev/null || true )
		if grep -q '_plugMain' <<<"$symbols"; then
			echo "   ${bundle_name}: exports _plugMain"
		else
			failures+=("${bundle_name}: no _plugMain export -- the host will load the bundle and find no plugins")
		fi
	else
		echo "   skipped ${bundle_name}: no universal build. Run:"
		echo "     cmake -B build-universal -DCMAKE_BUILD_TYPE=Release && cmake --build build-universal"
	fi
done

echo
if (( ${#failures[@]} == 0 )); then
	echo "all checks passed"
	exit 0
fi

echo "FAILURES:"
printf '  %s\n' "${failures[@]}"
exit 1
