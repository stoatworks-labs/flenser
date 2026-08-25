#pragma once

/**
	Host parameters are 0..1; these are what they mean.

	Every numeric parameter this plugin declares is a plain FF_TYPE_STANDARD
	float in 0..1, including the ones that stand for a count of cells or a
	number of turns per second. That is not a style preference.
	`CFFGLPluginManager::SetParamInfo` clamps a standard default into 0..1
	*before* returning, and `SetParamRange` can only be called afterwards
	because it finds the parameter by ID -- so a parameter declared in cells
	cannot declare a default in cells, and 24 would silently become 1. The
	conversions live here instead, in one file the FFGL build, the OpenFX
	build, the offline harness and the browser demo all read, so there is only
	ever one answer to what a slider position means.

	Where a mapping is **geometric** rather than linear it is because the
	interesting range is at one end. A slider is a fixed number of pixels
	wide, and spending half of them on a range nobody uses is the difference
	between a control that works and one with a sweet spot at 0.03. Cell size
	is the clearest case: the step from two cells filling the gate to four is
	a different picture, and the step from forty-six to forty-eight is not.

	Where a mapping is geometric **from a floor and then floored to zero** --
	Merge, Drift, Churn, Refraction -- it is because a geometric mapping
	cannot reach zero, and a control that never quite switches its effect off
	is a control that has to be fought rather than used. An operator who wants
	no refraction wants *none*, not a sixteenth of a pixel of it.

	Where a mapping is **bipolar** -- Spin, Temperature -- 0.5 is the null and
	the two halves are mirror images. Those are the controls where the
	interesting question is "which way", and a unipolar version of either
	would need a second Reverse checkbox to say the same thing worse.
*/
namespace flenser
{

//---------------------------------------------------------------------------
// The wheel: what is between the two glasses.
//---------------------------------------------------------------------------

/// 1 to 48 cells, geometrically, rounded.
///
/// Forty-eight is the ceiling because the field is evaluated by walking every
/// cell at every pixel, in both builds -- so this is the one control that is
/// linear in render cost, and the OpenFX build pays it on the CPU. A real
/// six-inch wheel holds a few dozen cells worth looking at; past that they
/// are smaller than the rim treatment and the picture reads as noise.
int CellsFromParam( float value );

/// 0.03 to 0.9 wheel units, geometrically. The radius of an average cell,
/// where 1.0 is half the picture's short edge.
float SizeFromParam( float value );

/// 0 to 1, linear. How much the hashed per-cell size spread deviates from
/// Size, as a fraction of it. At 1 the smallest cell is a tenth of the
/// largest, which is roughly what a dish that has been squeezed looks like.
float VariationFromParam( float value );

/// 0 to 0.5 wheel units, geometrically from a floor and zeroed at the bottom.
/// The fillet radius where two cells meet.
///
/// This is the control that decides whether the wheel is holding beads or
/// pools. At 0 the cells are separate discs that pass through each other; as
/// it rises they pull into one another with a proper meniscus fillet, which
/// is what oil in water actually does when the surface tension is beaten.
float MergeFromParam( float value );

/// 0 to 1.4 wheel units, linear. The radius of the disc the cells are packed
/// into. Past about 1.0 the outermost cells sit beyond the picture, which is
/// the normal way to run it -- a real wheel is bigger than the gate.
float SpreadFromParam( float value );

/// 0 to 1, linear. Blends the even golden-angle packing towards a hashed
/// swarm. At 0 the cells sit on a phyllotactic spiral, which is the closest
/// arrangement to a dish full of equal cells pressing on each other; at 1
/// they are scattered, which is what a dish that has just been rocked looks
/// like.
float ScatterFromParam( float value );

/// 1 to 9999, rounded. A different wheel.
int SeedFromParam( float value );

//---------------------------------------------------------------------------
// Motion.
//---------------------------------------------------------------------------

/// 0 to 1.5 cycles per second, linear, with zero meaning stopped.
///
/// Not geometric, and not bipolar. Linear because the useful range is the
/// whole of it -- a liquid wheel is worth watching anywhere between a crawl
/// and a rolling boil. Not bipolar because each cell travels its own closed
/// orbit and reversing them all is indistinguishable from a phase shift.
float SpeedFromParam( float value );

/// 0 to 0.5 wheel units, geometrically from a floor and zeroed at the bottom.
/// The radius of the convection orbit each cell travels.
float DriftFromParam( float value );

/// -0.25 to +0.25 turns per second, bipolar about 0.5. The whole wheel
/// turning in its holder, which is what the motorised ones do and what a
/// hand-held one does when it is being rolled.
float SpinFromParam( float value );

/// 0 to 0.6 **of the noise's own wavelength**, geometrically from a floor and
/// zeroed at the bottom. How far the noise field displaces the wheel before
/// the cells are evaluated -- the churn that stops them being circles.
///
/// A fraction of the wavelength rather than a distance, and the difference is
/// load-bearing: an absolute displacement that is large against the noise's
/// feature size folds the plane, at which point the field stops being a
/// distance field and every cell comes out as a nest of contour lines. Tying
/// the amplitude to Grain means Churn does the same kind of thing at every
/// Grain and cannot reach that regime. `WarpPoint` does the division.
float ChurnFromParam( float value );

/// 0.5 to 12 noise cells across the wheel, geometrically. Low is a slow swell
/// that moves whole cells; high is the boiling texture that appears when the
/// projector has been on for a few minutes.
float GrainFromParam( float value );

/// 0 to 2 cycles per second, linear, with zero meaning frozen. How fast the
/// noise field itself scrolls. Not bipolar: noise has no direction to
/// reverse.
float BoilFromParam( float value );

//---------------------------------------------------------------------------
// Optics: what the light does on its way through.
//---------------------------------------------------------------------------

/// 0 to 1, linear. How much of the lamp a cell's dye absorbs at its centre.
///
/// This is the control that makes the effect subtractive rather than
/// additive, and it is worth knowing which: overlapping cells MULTIPLY their
/// transmittances, so a cyan cell over a magenta one is blue and not white.
/// That is the whole difference in look between a dyed wheel and any number
/// of coloured blobs added together.
float DensityFromParam( float value );

/// 0 to 0.12 of the picture's short edge, geometrically from a floor and
/// zeroed at the bottom. How far the lamp is displaced where a cell's edge is
/// steepest -- the lens the meniscus makes.
float RefractionFromParam( float value );

/// 0 to 1, linear, used as a +/-30% differential on the red and blue
/// displacements. The colour fringe a thick edge of oil actually makes.
float DispersionFromParam( float value );

/// 0 to 1, linear. How dark the rim goes. Total internal reflection at a
/// steep meniscus sends light sideways instead of forward, and the edge of
/// every cell in a real projection is a dark line for exactly that reason.
float MeniscusFromParam( float value );

/// 0 to 2, linear. How bright the caustic line just inside the rim is, as a
/// multiple of the lamp behind it. Over 1 on purpose: a caustic is light that
/// has been *concentrated*, so it is genuinely brighter than the lamp, and
/// clipping there is what the eye reads as wet.
float CausticFromParam( float value );

/// 0.002 to 0.15 wheel units, geometrically. How wide the rim treatment is.
/// The floor is deliberately near a pixel: a hard edge is a legitimate look
/// and a control that cannot reach one is missing its most useful end.
float RimFromParam( float value );

//---------------------------------------------------------------------------
// Colour.
//---------------------------------------------------------------------------

/// 0 to 1 turn. Rotates the whole palette round the wheel.
float HueFromParam( float value );

/// 0 to 1 turn. How far apart the palette's hues sit. At 0 every cell is the
/// same colour, which is a wheel charged with one dye and is a real thing to
/// want.
float HueSpreadFromParam( float value );

/// 0 to 1, linear. How saturated the dyes are. Food colouring straight from
/// the bottle is at the top of this; a wheel that has been run for an hour is
/// not.
float SaturationFromParam( float value );

//---------------------------------------------------------------------------
// The lamp behind the glass.
//
// In the effect build the incoming clip is the lamp, so these controls
// describe what the projector does to it rather than what it is. They are
// deliberately not hidden there: a projector's hot spot, its colour
// temperature and its gate are things the clip goes through, and an operator
// wanting the clip untouched has Density, Refraction and Hotspot at zero.
//---------------------------------------------------------------------------

/// 0 to 2, linear. Lamp brightness. Over 1 because a liquid wheel eats light:
/// a dense dye at full saturation passes maybe a fifth of it, and needing to
/// push past unity to get a picture back is the accurate behaviour.
float LampFromParam( float value );

/// 0 to 1, linear. How much brighter the middle of the gate is than the edge.
/// An overhead projector's condenser is a Fresnel lens and its hot spot is
/// the first thing anybody notices about the format.
float HotspotFromParam( float value );

/// -1 to +1, bipolar about 0.5. Negative is the cold blue-white of a metal
/// halide head; positive is the amber of a tungsten overhead projector that
/// has been dimmed.
float TemperatureFromParam( float value );

/// 0.2 to 2.6 wheel units, geometrically -- and **off** at the very top of
/// the travel, where it returns `kGateOff`.
///
/// The radius of the circular gate. Below 1 the disc and its edge are the
/// picture, which is how an overhead projector with a clock glass on it
/// looks; above it the gate creeps out past the frame.
///
/// The explicit off is the same argument as the zeroed floors above, at the
/// other end. The corner of a 16:9 frame is 2.04 wheel units out and a 21:9
/// one is 2.54, so a control that merely got large would still be cutting the
/// corners off a wide canvas at its maximum -- and "the plugin rounds off my
/// corners and the control that should stop it is already at the top" is not
/// something an operator should have to work out. Off is off.
float GateFromParam( float value );

/// What `GateFromParam` returns when the gate is off: a radius no frame
/// reaches, so `GateAt` is exactly 1 everywhere. Named rather than spelled
/// out because the harness's null check asserts on it.
constexpr float kGateOff = 1.0e6f;

/// 0 to 1, linear. How soft the gate's edge is, as a fraction of its radius.
/// At 0 it is the hard circle of a projector that is in focus.
float GateSoftFromParam( float value );

//---------------------------------------------------------------------------
// Simmer: the one part of this plugin that remembers anything.
//
// FFGL only, and off by default. See Flenser.h for why it cannot exist in
// the OpenFX build.
//---------------------------------------------------------------------------

/// 0 to 1, linear, default 0. How much of the previous frame is carried
/// forward, advected along the churn field.
float SimmerFromParam( float value );

/// 0 to 0.04 of the picture's short edge, linear. How far a frame is dragged
/// along the flow before it is carried forward.
float SmearFromParam( float value );

//---------------------------------------------------------------------------
// Options. These take the option's stored VALUE, not its position in the
// host's list -- the lists are declared alphabetically and the two are not
// the same thing. See the declaration in Flenser.cpp.
//---------------------------------------------------------------------------

/// The dye set, as a `Palette`.
int PaletteFromParam( float optionValue );

/// How the oil meets the clip, as a `LampMode`. Effect build only; the
/// generator ignores it.
int ModeFromParam( float optionValue );

} // namespace flenser
