#pragma once

#include <cstdint>

/**
	The wheel: what an oil-and-alcohol projector actually does to light, as
	arithmetic.

	---------------------------------------------------------------------
	What is being modelled
	---------------------------------------------------------------------

	Two watch glasses clamped face to face with a few millilitres of oil,
	water, alcohol and dye between them, sitting on the stage of an overhead
	projector. The lamp is under it, the mirror and lens are over it, and the
	operator squeezes the glasses and lets the heat do the rest. That is a
	liquid light show, and it has four separable behaviours:

	1. **Immiscible cells.** Oil in dyed water does not mix. It forms rounded
	   cells that press on each other, and where two press hard enough they
	   join with a *fillet* rather than a crossing. `Merge` is that fillet.

	2. **Subtractive colour.** Each cell is a dye filter. Light going through
	   two of them is multiplied twice, so cyan over magenta is BLUE. Nearly
	   every "blob" effect adds instead, and adding is what makes them look
	   like lava lamps rather than like this. See `Transmittance` below --
	   getting this one right is most of the difference.

	3. **The meniscus is a lens.** The edge of a cell is a curved oil-water
	   boundary a few tenths of a millimetre across, and it is the only part
	   of the wheel doing serious optics: it displaces what is behind it,
	   splits it slightly by wavelength, throws a bright caustic line just
	   inside the edge and a dark line at it. Nothing in the middle of a cell
	   bends light much at all, which is why `Refraction` here is an edge
	   band and not a whole-cell distortion.

	4. **The projector.** A Fresnel condenser with a visible hot spot, a
	   tungsten or halide lamp with a colour temperature, and a round gate.

	---------------------------------------------------------------------
	The split that makes it checkable
	---------------------------------------------------------------------

	**Per cell**, on the CPU, in both builds: `CellAt` takes an index and the
	wheel's settings and returns where that cell is, how big it is and what
	colour its dye is. It never looks at a pixel. At most forty-eight calls a
	frame, so both builds simply call it and hand the GPU the answers as
	uniforms -- the arrangement exists **once** and cannot drift between
	builds.

	**Per pixel**, written twice: `WarpPoint`, `FieldAt`, `SynthLamp` and
	`GateAt` below, and their transcriptions in `kOilLibrary` in Shaders.cpp.
	They have to be written twice because the GPU cannot call C++ and the
	OpenFX build cannot call GLSL. Every mirrored line carries a
	`//= mirrored` marker in both files, and `fltest --field` compares the two
	numerically over every control. Nothing else notices when they drift.

	This is the same division afterglow uses for its decay model, and for the
	same reason: a model that cannot drift is worth more than one that is
	checked.

	---------------------------------------------------------------------
	Why it is closed form, and where that stops
	---------------------------------------------------------------------

	Everything above is a pure function of position and time. No advection, no
	accumulator, no frame history. That is not the most authentic way to boil
	oil -- a real dish is a fluid simulation and this is not one -- but it
	buys three things a simulation cannot:

	- **Any frame renders on its own.** OpenFX hosts render frames in any
	  order, alone, on several threads at once. Resolve rendering frame 500
	  before frame 4 is normal, and a stateful effect either serialises the
	  host or renders differently every time.
	- **The GPU and the CPU can be proved to agree**, because there is a
	  right answer that does not depend on how many frames have gone past.
	- **Scrubbing works.** An operator dragging the playhead sees the wheel at
	  that moment, not the wheel restarting.

	`Simmer` is the deliberate exception and it is confined to the FFGL build:
	see Flenser.h.

	---------------------------------------------------------------------
	Units
	---------------------------------------------------------------------

	**Wheel space.** The picture's short edge runs -1..1, the long edge runs
	-aspect..aspect, y is DOWN to match the uv convention on both sides, and
	the gate is centred on the origin. Cells are round in this space, which is
	the whole point of it: a cell radius expressed in uv would be an ellipse
	on anything that is not square, and a 21:9 canvas would show egg-shaped
	oil.

	**Time is seconds** on both sides. The FFGL build converts the host's
	clock (which is milliseconds in Resolume and undocumented in the header);
	the OpenFX build divides the frame number by the clip's frame rate.
*/
namespace flenser
{

/// The dye set on the wheel.
///
/// A palette is a small table of hue offsets in turns, and a cell picks one
/// by hash. The offsets are scaled by Hue Spread and rotated by Hue, so the
/// palette decides the *relationship* between the dyes and the two controls
/// decide where they sit and how far apart.
enum class Palette
{
	Aniline = 0,///< the food-colouring bottle: red, green, blue, yellow
	Ink,        ///< printing inks: cyan, magenta, yellow. The subtractive primaries.
	Sodium,     ///< the hot end only: red through orange to amber
	Spectrum,   ///< continuous, hashed anywhere on the wheel
	Duotone,    ///< two complementary dyes and nothing between them
	Mono,       ///< one dye
	Count
};

/// What the incoming clip is, in the effect build. The generator ignores it.
enum class LampMode
{
	Project = 0,///< the clip IS the lamp: it is seen through the oil
	Over,       ///< the oil is lit by its own lamp and sits over the clip
	Colourise,  ///< the clip's brightness drives the lamp; the dye is all the colour
	Count
};

const char* PaletteName( Palette value );
const char* LampModeName( LampMode value );

//---------------------------------------------------------------------------
/// One cell of oil, resolved for one frame.
///
/// Everything the per-pixel stage needs to know about a cell, and nothing
/// else. Handed to the GPU as four uniform arrays.
//---------------------------------------------------------------------------
struct Cell
{
	float x = 0.0f;///< centre, wheel space
	float y = 0.0f;
	float radius = 0.1f;

	/// The dye's **transmittance**, not its appearance: what fraction of each
	/// channel this cell lets through at full density. A magenta dye is
	/// (1, 0, 1) because it passes red and blue and stops green.
	float dye[ 3 ] = { 1.0f, 1.0f, 1.0f };
};

/// The largest wheel the plugin will hold. The field is evaluated by walking
/// every cell at every pixel in both builds, so this is a hard cost ceiling
/// as well as a control range -- and on the OpenFX side it is paid on the
/// CPU.
constexpr int kMaxCells = 48;

//---------------------------------------------------------------------------
/// The wheel's settings for one frame, in physical units.
///
/// Filled from the host's 0..1 parameters through Controls.h by whichever
/// build is running, so the conversions happen exactly once per frame and in
/// exactly one place.
//---------------------------------------------------------------------------
struct Wheel
{
	//-- what is on the glass ------------------------------------------
	int cells       = 24;
	float size      = 0.18f;
	float variation = 0.35f;
	float merge     = 0.06f;
	float spread    = 0.9f;
	float scatter   = 0.25f;
	int seed        = 1;

	//-- how it moves ---------------------------------------------------
	float speed = 0.18f;
	float drift = 0.12f;
	float spin  = 0.0f;
	float churn = 0.09f;
	float grain = 2.4f;
	float boil  = 0.35f;

	//-- what the light does --------------------------------------------
	float density    = 0.8f;
	float refraction = 0.02f;
	float dispersion = 0.3f;
	float meniscus   = 0.5f;
	float caustic    = 0.6f;
	float rim        = 0.02f;

	//-- colour ----------------------------------------------------------
	Palette palette  = Palette::Aniline;
	float hue        = 0.0f;
	float hueSpread  = 0.7f;
	float saturation = 0.85f;

	//-- the lamp --------------------------------------------------------
	float lamp        = 1.0f;
	float hotspot     = 0.35f;
	float temperature = 0.15f;
	float gate        = 1.5f;
	float gateSoft    = 0.25f;

	//-- the frame -------------------------------------------------------
	float aspect = 16.0f / 9.0f;///< width / height

	/// Seconds. The clock the phases below are derived from; `CellAt` itself
	/// never reads it.
	float time = 0.0f;

	/// Turns of cell orbit, and turns of wheel rotation. **Not** `time *
	/// speed` and `time * spin` — see `SetFreeRunningPhases` for when they
	/// are exactly that, and `Flenser.h` for when they are not and why.
	float orbitPhase = 0.0f;
	float spinPhase  = 0.0f;

	/// Seconds of *boil*, which is not the same number.
	///
	/// The FFGL build integrates the Boil rate rather than multiplying by it,
	/// so that nudging Boil live changes what happens next instead of
	/// rescaling the whole history and jumping the field to a different
	/// place -- worst at exactly the moment somebody is watching the control
	/// they are moving. The OpenFX build cannot integrate anything (there is
	/// no previous frame to have integrated from) and uses `time * boil`,
	/// which is deterministic, which matters more in a host that renders
	/// frames out of order. Documented in AGENTS.md as the one departure
	/// between the builds' pictures.
	float boilPhase = 0.0f;
};

//---------------------------------------------------------------------------
/// Where cell `index` is this frame, how big it is, and what colour it dyes.
///
/// CPU only, in both builds, at most `kMaxCells` times a frame. Nothing in
/// here looks at a pixel, which is what makes the arrangement checkable
/// against a table of numbers rather than against a picture.
///
/// The base arrangement is a **golden-angle spiral** -- the phyllotactic
/// packing -- because that is the closest simple arrangement to a dish full
/// of equal cells pressing on each other, and because it stays even at every
/// count. A uniform hashed scatter looks like a dish that has just been
/// shaken; Scatter blends between the two.
//---------------------------------------------------------------------------
Cell CellAt( int index, const Wheel& wheel );

//---------------------------------------------------------------------------
/// Fill `orbitPhase` and `spinPhase` as a pure function of `time` — the
/// replayable form, `time * rate`.
///
/// Right wherever a frame must be reproducible from its timestamp alone: the
/// OpenFX builds, which render out of order and re-render on every seek, the
/// browser demo, which is scrubbable, and the offline cell dump.
///
/// **Wrong in a live host**, where the operator moves Speed and Spin while
/// watching. `time * rate` rescales the whole history, so an hour into a
/// composition a small nudge is worth hundreds of turns and the wheel
/// teleports. The FFGL build anchors the phases instead — `Flenser.h`.
//---------------------------------------------------------------------------
void SetFreeRunningPhases( Wheel& wheel );

//===========================================================================
// The per-pixel stage. Everything below this line is mirrored in
// `kOilLibrary` in Shaders.cpp, line for line, and compared by
// `fltest --field`. Change one, change both.
//===========================================================================

/// Value noise on the integer lattice, from the PCG hash, with a quintic
/// fade. In -1..1.
///
///= mirrored
float Noise2( float x, float y );

/// The churn. Displaces a point in wheel space before any cell is evaluated,
/// by two counter-scrolling octaves of `Noise2`.
///
/// Two octaves going opposite ways rather than one scrolling octave, because
/// a single translated noise field *slides* -- the pattern is rigid and the
/// eye reads it as a moving texture rather than as a fluid. Counter-scrolling
/// layers change shape as they pass through each other, which is what boiling
/// looks like.
///
///= mirrored
void WarpPoint( float x, float y, float churn, float grain, float boilPhase,
                float& outX, float& outY );

/// What one pixel of the field is.
struct Sample
{
	/// Signed distance to the merged surface of the whole wheel, in wheel
	/// units. Negative inside oil.
	float d = 1.0f;

	/// The surface normal, pointing OUT of the oil. Accumulated alongside `d`
	/// through the same smooth-minimum weights rather than taken by finite
	/// differences, which would cost four more evaluations of every cell at
	/// every pixel.
	///
	/// It is the exact gradient of the `min` and an approximation of the
	/// gradient of the *smooth* min -- the fillet's own curvature term is
	/// dropped. That is a deliberate simplification and not an error, and it
	/// is the identical simplification on both sides, so the mirror check
	/// still means what it says.
	float gx = 0.0f;
	float gy = 0.0f;

	/// The signed distance to the NEAREST single cell boundary -- the `di`
	/// with the smallest magnitude, not the smallest value.
	///
	/// This, and not `d`, is what the rim treatment is drawn from, and the
	/// difference is the whole look. `d` is the union's surface, so a rim
	/// drawn from it appears only on the OUTER silhouette of the wheel: a
	/// dozen cells piled together come out as one flat coloured shape with a
	/// single meniscus round the outside of the pile. Every cell's edge is a
	/// real oil-water boundary and every one of them is visible in a real
	/// projection, including where one cell lies over another -- they are
	/// stacked in the few tenths of a millimetre between the glasses, not
	/// fused.
	///
	/// So the two are kept apart on purpose: `d` fillets and is what Merge
	/// acts on, and is what the silhouette and the normal come from; `dn`
	/// does not fillet and is where the light actually meets a surface.
	float dn = 1.0f;

	/// The dye stack's transmittance at this pixel: the product over every
	/// cell covering it. **Multiplied, not added** -- see the note at the top
	/// of this file. In 0..1 per channel.
	float t[ 3 ] = { 1.0f, 1.0f, 1.0f };
};

/// Evaluate the whole wheel at one point in wheel space.
///
/// One pass over the cells, accumulating three things at once: the merged
/// distance, the normal, and the dye product. Three separate loops would read
/// better and cost three times as much, and this is the innermost loop of
/// both builds.
///
///= mirrored
Sample FieldAt( float x, float y, const Cell* cells, int count,
                float merge, float density, float rim );

/// The projector's own lamp, at a point in wheel space, before the oil. RGB.
///
///= mirrored
void SynthLamp( float x, float y, float lamp, float hotspot, float temperature,
                float& outR, float& outG, float& outB );

/// The round gate, at a point in wheel space. 1 inside, 0 outside.
///
///= mirrored
float GateAt( float x, float y, float gate, float gateSoft );

/// The two rim profiles, from the NEAREST boundary's signed distance (`dn`,
/// not `d` -- see the note on the field) and the rim width:
/// `outCaustic` peaks just INSIDE the surface (converged light) and
/// `outMeniscus` peaks ON it (light lost sideways to total internal
/// reflection). Both in 0..1.
///
///= mirrored
void RimProfiles( float d, float rim, float& outCaustic, float& outMeniscus );

/// How far, and in which direction, the meniscus displaces what is behind it
/// -- in wheel units. Concentrated in a band on the surface, because that is
/// where a real oil-water boundary has any curvature at all.
///
///= mirrored
void BendAt( const Sample& s, float refraction, float rim, float& outX, float& outY );

//===========================================================================
// End of the mirrored section.
//===========================================================================

/// HSV to RGB, for the dye colours. CPU only -- it runs per cell, in
/// `CellAt`, and its answers arrive at the GPU as uniforms.
void HsvToRgb( float h, float s, float v, float& outR, float& outG, float& outB );

/// Wheel space from uv, and back. Trivial, and here rather than at three call
/// sites so that the y convention is stated once.
inline void UvToWheel( float u, float v, float aspect, float& x, float& y )
{
	x = ( u - 0.5f ) * 2.0f * aspect;
	y = ( v - 0.5f ) * 2.0f;
}

inline void WheelToUv( float x, float y, float aspect, float& u, float& v )
{
	u = x / ( 2.0f * aspect ) + 0.5f;
	v = y * 0.5f + 0.5f;
}

} // namespace flenser
