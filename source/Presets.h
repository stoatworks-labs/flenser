#pragma once

/**
	Factory presets: named wheels an operator can reach in one gesture.

	The values live in the same 0..1 parameter space both builds expose (the
	FFGL and OpenFX builds deliberately share it), so ONE table drives both
	and a preset looks identical in Resolume and in Resolve. Plain data only;
	the application machinery lives with each host's glue.

	Element 0 of the host-facing dropdown is "Custom" and is not in this
	table: it means "the sliders are the truth".

	**What a preset covers is the wheel and the lamp** -- what the thing looks
	like. It deliberately does not cover:

	- **Seed.** The arrangement is how an operator gets a different wheel out
	  of the same look, and a preset that reset it would take that away.
	- **Mix**, which is the operator's way of pulling any of it back.
	- **Mode**, which is a statement about the edit rather than about the
	  picture: a preset that quietly moved a shot from Project to Over would
	  change what the clip is doing in the composition.
	- **Simmer and Smear**, which do not exist in the OpenFX build at all (see
	  Flenser.h). A shared table cannot carry a control only one of its two
	  readers has, and faking it in the other would be worse than the gap.
*/

namespace flenser
{
namespace presets
{
/// The parameters a preset sets, in one fixed order. The FFGL build binds
/// this order to its ParamIDs and the OpenFX build to its param handles; both
/// static_assert against kParamCount so the three lists cannot drift apart
/// silently.
enum Param
{
	kCells,
	kSize,
	kVariation,
	kMerge,
	kSpread,
	kScatter,
	kSpeed,
	kDrift,
	kSpin,
	kChurn,
	kGrain,
	kBoil,
	kDensity,
	kRefraction,
	kDispersion,
	kMeniscus,
	kCaustic,
	kRim,
	kPalette,
	kHue,
	kHueSpread,
	kSaturation,
	kLamp,
	kHotspot,
	kTemperature,
	kGate,
	kGateSoft,
	kParamCount
};

struct Preset
{
	const char* name;
	float v[ kParamCount ];
};

// Palette is an option VALUE, not a list position: 0 Aniline, 1 Ink,
// 2 Sodium, 3 Spectrum, 4 Duotone, 5 Mono. Everything else is a slider in
// 0..1 and Controls.cpp says what each one means in physical units; Spin and
// Temperature are bipolar, so 0.5 is their null.
//
// The physical value of every number below can be read out with
//     ./build/fltest --preset "<name>" --list
// which is how they were tuned and how a change to a mapping in Controls.cpp
// gets checked against them.
inline constexpr Preset kPresets[] = {
	// The DIY article, and the one to start from: a clock glass on an
	// overhead projector. A handful of big pools with a heavy fillet where
	// they meet, barely moving, warm tungsten light and the round gate of the
	// glass itself well inside the frame.
	//
	// Note that this is NOT the plugin's own defaults. A factory preset whose
	// values are the defaults provably changes nothing when it is applied
	// over them, which makes it indistinguishable from a preset that is
	// broken -- tools/sweep.py found exactly that in tinsel.
	{ "Overhead Projector",
	  { /*Cells*/ 0.537f, /*Size*/ 0.677f, /*Var*/ 0.30f, /*Merge*/ 0.810f, /*Spread*/ 0.500f, /*Scatter*/ 0.20f,
	    /*Speed*/ 0.067f, /*Drift*/ 0.583f, /*Spin*/ 0.540f, /*Churn*/ 0.880f, /*Grain*/ 0.346f, /*Boil*/ 0.150f,
	    /*Density*/ 0.85f, /*Refract*/ 0.686f, /*Disp*/ 0.35f, /*Menisc*/ 0.55f, /*Caustic*/ 0.35f, /*Rim*/ 0.533f,
	    /*Palette*/ 0, /*Hue*/ 0.00f, /*HueSpread*/ 0.85f, /*Sat*/ 0.80f,
	    /*Lamp*/ 0.560f, /*Hotspot*/ 0.55f, /*Temp*/ 0.700f, /*Gate*/ 0.653f, /*GateSoft*/ 0.30f } },

	// The purpose-built unit: a motorised wheel, more cells and smaller ones,
	// evenly packed, filling the frame with no gate in sight, under a cold
	// discharge lamp. Cleaner and more mechanical than the overhead, which is
	// exactly what the machine is for.
	{ "Optikinetics",
	  { /*Cells*/ 0.821f, /*Size*/ 0.473f, /*Var*/ 0.25f, /*Merge*/ 0.523f, /*Spread*/ 0.786f, /*Scatter*/ 0.10f,
	    /*Speed*/ 0.133f, /*Drift*/ 0.417f, /*Spin*/ 0.600f, /*Churn*/ 0.800f, /*Grain*/ 0.506f, /*Boil*/ 0.200f,
	    /*Density*/ 0.80f, /*Refract*/ 0.564f, /*Disp*/ 0.30f, /*Menisc*/ 0.50f, /*Caustic*/ 0.45f, /*Rim*/ 0.373f,
	    /*Palette*/ 0, /*Hue*/ 0.10f, /*HueSpread*/ 0.90f, /*Sat*/ 0.90f,
	    /*Lamp*/ 0.550f, /*Hotspot*/ 0.20f, /*Temp*/ 0.350f, /*Gate*/ 1.000f, /*GateSoft*/ 0.15f } },

	// What happens the moment the alcohol goes in: the surface tension
	// collapses, the cells tear into ribbons and the whole dish writhes. High
	// churn, a fast boil, small cells and a hard bright rim.
	{ "Alcohol Burn",
	  { /*Cells*/ 0.895f, /*Size*/ 0.354f, /*Var*/ 0.65f, /*Merge*/ 0.667f, /*Spread*/ 0.786f, /*Scatter*/ 0.55f,
	    /*Speed*/ 0.400f, /*Drift*/ 0.709f, /*Spin*/ 0.500f, /*Churn*/ 0.980f, /*Grain*/ 0.654f, /*Boil*/ 0.600f,
	    /*Density*/ 0.75f, /*Refract*/ 0.807f, /*Disp*/ 0.60f, /*Menisc*/ 0.65f, /*Caustic*/ 0.70f, /*Rim*/ 0.254f,
	    /*Palette*/ 0, /*Hue*/ 0.00f, /*HueSpread*/ 1.00f, /*Sat*/ 0.95f,
	    /*Lamp*/ 0.625f, /*Hotspot*/ 0.30f, /*Temp*/ 0.500f, /*Gate*/ 0.875f, /*GateSoft*/ 0.20f } },

	// Printing ink dropped into water: the subtractive primaries, thick, in
	// big slow plumes that pass through each other and go dark where they
	// cross. This is the preset that shows what the dye stack is doing --
	// cyan over magenta is blue here, and it is white in every effect that
	// adds its blobs instead.
	{ "Ink in Water",
	  { /*Cells*/ 0.642f, /*Size*/ 0.586f, /*Var*/ 0.45f, /*Merge*/ 0.926f, /*Spread*/ 0.643f, /*Scatter*/ 0.40f,
	    /*Speed*/ 0.100f, /*Drift*/ 0.709f, /*Spin*/ 0.480f, /*Churn*/ 0.940f, /*Grain*/ 0.250f, /*Boil*/ 0.250f,
	    /*Density*/ 1.00f, /*Refract*/ 0.443f, /*Disp*/ 0.20f, /*Menisc*/ 0.40f, /*Caustic*/ 0.25f, /*Rim*/ 0.694f,
	    /*Palette*/ 1, /*Hue*/ 0.00f, /*HueSpread*/ 1.00f, /*Sat*/ 1.00f,
	    /*Lamp*/ 0.650f, /*Hotspot*/ 0.25f, /*Temp*/ 0.500f, /*Gate*/ 1.000f, /*GateSoft*/ 0.10f } },

	// The hot end of the palette only, dense, with the condenser's hot spot
	// wide open. A wheel charged with one bottle of red and left under the
	// lamp until it has gone amber in the middle.
	{ "Sodium",
	  { /*Cells*/ 0.716f, /*Size*/ 0.473f, /*Var*/ 0.55f, /*Merge*/ 0.667f, /*Spread*/ 0.643f, /*Scatter*/ 0.35f,
	    /*Speed*/ 0.133f, /*Drift*/ 0.583f, /*Spin*/ 0.520f, /*Churn*/ 0.900f, /*Grain*/ 0.346f, /*Boil*/ 0.200f,
	    /*Density*/ 0.95f, /*Refract*/ 0.564f, /*Disp*/ 0.15f, /*Menisc*/ 0.60f, /*Caustic*/ 0.55f, /*Rim*/ 0.533f,
	    /*Palette*/ 2, /*Hue*/ 0.00f, /*HueSpread*/ 1.00f, /*Sat*/ 0.90f,
	    /*Lamp*/ 0.700f, /*Hotspot*/ 0.75f, /*Temp*/ 0.800f, /*Gate*/ 0.740f, /*GateSoft*/ 0.45f } },

	// No dye at all: water and oil, and nothing but the optics. Every mark on
	// the screen here is refraction, a caustic or a meniscus line, which
	// makes it the preset to reach for when the question is what the Optics
	// group actually does -- and, in the effect build, the one that reads
	// most like a real lens over the clip.
	{ "Clear Water",
	  { /*Cells*/ 0.821f, /*Size*/ 0.473f, /*Var*/ 0.50f, /*Merge*/ 0.523f, /*Spread*/ 0.786f, /*Scatter*/ 0.30f,
	    /*Speed*/ 0.233f, /*Drift*/ 0.583f, /*Spin*/ 0.500f, /*Churn*/ 0.860f, /*Grain*/ 0.506f, /*Boil*/ 0.350f,
	    /*Density*/ 0.30f, /*Refract*/ 0.807f, /*Disp*/ 0.75f, /*Menisc*/ 0.70f, /*Caustic*/ 0.80f, /*Rim*/ 0.373f,
	    /*Palette*/ 5, /*Hue*/ 0.00f, /*HueSpread*/ 0.00f, /*Sat*/ 0.00f,
	    /*Lamp*/ 0.500f, /*Hotspot*/ 0.15f, /*Temp*/ 0.450f, /*Gate*/ 1.000f, /*GateSoft*/ 0.10f } },

	// Merge at zero: the cells stay separate beads and pass straight through
	// one another instead of joining. Not what a real dish does, and the most
	// useful setting the control has -- a field of hard-edged coloured discs
	// is a mask, a pixel-map driver and a title background.
	{ "Beading",
	  { /*Cells*/ 0.953f, /*Size*/ 0.204f, /*Var*/ 0.80f, /*Merge*/ 0.000f, /*Spread*/ 0.929f, /*Scatter*/ 0.75f,
	    /*Speed*/ 0.267f, /*Drift*/ 0.834f, /*Spin*/ 0.560f, /*Churn*/ 0.700f, /*Grain*/ 0.830f, /*Boil*/ 0.400f,
	    /*Density*/ 1.00f, /*Refract*/ 0.443f, /*Disp*/ 0.25f, /*Menisc*/ 0.35f, /*Caustic*/ 0.60f, /*Rim*/ 0.150f,
	    /*Palette*/ 3, /*Hue*/ 0.00f, /*HueSpread*/ 1.00f, /*Sat*/ 1.00f,
	    /*Lamp*/ 0.550f, /*Hotspot*/ 0.10f, /*Temp*/ 0.500f, /*Gate*/ 1.000f, /*GateSoft*/ 0.05f } },

	// Three enormous cells nearly filling the gate, moving as slowly as the
	// controls allow, with a wide soft rim. Almost a colour wash -- for
	// putting under something rather than for looking at.
	{ "Slow Bloom",
	  { /*Cells*/ 0.284f, /*Size*/ 0.881f, /*Var*/ 0.20f, /*Merge*/ 0.926f, /*Spread*/ 0.357f, /*Scatter*/ 0.15f,
	    /*Speed*/ 0.047f, /*Drift*/ 0.709f, /*Spin*/ 0.515f, /*Churn*/ 0.930f, /*Grain*/ 0.150f, /*Boil*/ 0.100f,
	    /*Density*/ 0.70f, /*Refract*/ 0.686f, /*Disp*/ 0.40f, /*Menisc*/ 0.30f, /*Caustic*/ 0.30f, /*Rim*/ 0.850f,
	    /*Palette*/ 4, /*Hue*/ 0.55f, /*HueSpread*/ 0.60f, /*Sat*/ 0.75f,
	    /*Lamp*/ 0.600f, /*Hotspot*/ 0.40f, /*Temp*/ 0.650f, /*Gate*/ 0.875f, /*GateSoft*/ 0.60f } },
};

inline constexpr int kCount = int( sizeof( kPresets ) / sizeof( kPresets[ 0 ] ) );

} // namespace presets
} // namespace flenser
