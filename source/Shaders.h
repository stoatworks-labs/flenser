#pragma once

/**
	The passes, as GLSL source.

	Three fragment shaders and one vertex shader:

	1. **oil**       output size, into a buffer of ours. The whole effect:
	                 warp the point, walk the cells, bend the lamp through the
	                 meniscus, stack the dyes, gate it. Everything expensive
	                 happens here and it happens once.
	2. **simmer**    output size, ping-ponged. The optional feedback: the
	                 previous frame advected along the churn and carried
	                 forward under a ceiling. Skipped entirely when Simmer is
	                 zero, which is the default and most of the frame's cost
	                 when it is not.
	3. **composite** to the host's own framebuffer. Blends the simmer buffer
	                 in and mixes against the untouched clip.

	`kOilLibrary` is a transcription of the per-pixel half of `Oil.cpp` --
	`Noise2`, `WarpPoint`, `FieldAt`, `SynthLamp`, `GateAt`, `RimProfiles` and
	`BendAt`. Every mirrored line carries a `//= mirrored` marker in both
	files.

	The per-*cell* half is not here at all: `CellAt` runs on the CPU in both
	builds and its answers arrive as two `vec3` uniform arrays. That is the
	whole reason this library is seven functions long rather than the whole
	effect -- see the note at the top of `Oil.h`.

	**The oil shader is assembled, not written out.** `OilShaderSource()`
	wraps the library in the pass, `SimmerShaderSource()` wraps it in the
	feedback pass, and `FieldProbeShaderSource()` wraps the same string in a
	one-sample-per-pixel probe for `fltest --field`. So the test runs the
	exact text the plugin runs. A test that compiled its own transcription of
	the library would agree with itself perfectly and prove nothing -- which
	is a real failure mode and not a hypothetical one: it is what "the demo
	looked right and behaved differently" turned out to be elsewhere in the
	fleet.
*/

#include <string>

namespace flenser
{

extern const char* const kVertexShader;
extern const char* const kCompositeShader;

/// The per-pixel oil stage, as GLSL. Not a complete shader: no `#version`
/// and no `main`.
extern const char* const kOilLibrary;

/// The main pass, assembled around kOilLibrary.
std::string OilShaderSource();

/// The feedback pass, assembled around kOilLibrary (it needs `WarpPoint` to
/// know which way to drag the previous frame).
std::string SimmerShaderSource();

/// One sample point per pixel and one mirrored function per draw, written to
/// a float target so the results can be read straight back and compared
/// against `Oil.cpp`. Built from the same kOilLibrary the oil pass uses.
///
/// `ProbeSlot` selects which four numbers a draw writes:
///
///   0  the field:      d, gradient.x, gradient.y, gate
///   1  the dye stack:  transmittance.rgb, Noise2 at the sample point
///   2  the rim:        caustic, meniscus, bend.x, bend.y
///   3  the lamp:       lamp.rgb, 0
///   4  the churn:      warped.x, warped.y, nearest boundary, 0
///
/// This exists only for `fltest --field`, and lives here rather than in the
/// harness so that there is one place where the library is concatenated into
/// something runnable.
std::string FieldProbeShaderSource();

/// Where the probe samples, given a pixel and the probe target's size. The
/// harness and the shader have to agree about this exactly, so it is stated
/// once, here, and both sides call it.
void ProbePoint( int px, int py, int width, int height, float spanX, float spanY,
                 float& outX, float& outY );

} // namespace flenser
