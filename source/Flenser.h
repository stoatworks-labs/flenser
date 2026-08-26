#pragma once

#include "Oil.h"
#include "PassBuffer.h"
#include "Presets.h"
#include "StoatworksAboutParams.h"

#include <FFGLSDK.h>

#include <string>

/**
	Flenser -- a liquid light show, as a plugin.

	**What it is.** Two watch glasses with oil, water, alcohol and dye between
	them, on the stage of an overhead projector. Cells of oil press on each
	other and join with a fillet; each one is a dye filter, so overlapping
	cells MULTIPLY their colours; the meniscus at each edge is a lens that
	displaces what is behind it, splits it by wavelength, and throws a bright
	caustic just inside a dark rim. The whole thing sits behind a Fresnel
	condenser with a visible hot spot and a round gate.

	The physics, such as it is, lives in `Oil.h`, and so does the argument for
	why the model is closed form. Read that first.

	**It ships twice from one class.** `FlenserPlugin( false )` is the
	generator -- the wheel with the plugin's own lamp behind it. Passed
	`true`, the same class takes an input and the clip becomes the lamp: the
	footage is what shines through the oil. Both bundles declare exactly the
	same parameters, which is deliberate -- every control means something in
	both, a projector's hot spot and colour temperature apply to a clip as
	readily as to a lamp, and a plugin whose sliders move around depending on
	which of two bundles you loaded is a plugin whose presets and saved
	compositions cannot be shared.

	**Three passes**, in `Shaders.h`: `oil` does everything and writes to a
	buffer, `simmer` optionally carries a little of the last frame forward,
	and `composite` mixes the result against the untouched clip. The middle
	one is skipped whenever Simmer is zero, which is the default.

	---------------------------------------------------------------------
	Simmer, and why it exists here and not in the OpenFX build
	---------------------------------------------------------------------

	Everything else in this plugin is a pure function of position and time.
	Simmer is not: it is a feedback buffer, advected along the churn field and
	carried forward under a ceiling.

	It is here because there is a thing a closed-form field genuinely cannot
	do, and it is the thing a real dish does constantly: leave a trail behind
	a moving cell. Oil that has just been somewhere is still slightly there.
	No function of (position, time) has that property, because it needs to
	know where the oil *was*.

	It is FFGL-only for the reason stated at the top of `Oil.h` and worth
	repeating: OpenFX hosts render frames in any order, alone, on several
	threads at once. Resolve rendering frame 500 before frame 4 is normal. A
	feedback buffer in that host either serialises it or renders differently
	every time -- and an effect that does not match its own preview on the
	second export is worse than an effect that is missing a control.

	So the OpenFX build does not have it, does not claim to, and says so in
	its own plugin description. What it costs is documented in
	`docs/USER-GUIDE.md` rather than papered over.

	Two properties keep it honest here:

	- **At Simmer 0 the picture is `oil` exactly.** The composite is
	  `mix( oil, max( oil, feed ), Simmer )`, so the null is not "almost the
	  same" -- it is the same numbers. That is what lets the pass be skipped
	  without the picture jumping the instant it is switched on.
	- **It cannot run away.** The feedback buffer is a `max` against the live
	  frame with a persistence strictly below 1, so nothing in it can ever be
	  brighter than the brightest oil that has been through it. A feedback
	  loop with a gain of 1.02 is a white screen four hundred frames later, in
	  the middle of a show, and no amount of care with the default value
	  prevents that; a ceiling does.
*/
class FlenserPlugin : public CFFGLPlugin
{
public:
	/// Clock test hook. The offline harness DECLARES its unit rather than
	/// leaving the calibration to infer one -- an absolute time handed over in
	/// a single frame is genuinely ambiguous, and an implicit unit is what let
	/// the millisecond bug through elsewhere in the fleet.
	void SetClockScaleForTest( double scale );

	explicit FlenserPlugin( bool overInput );

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	/// Test hook: the parameter ids a preset covers, in presets::Param order.
	/// Handed out rather than copied into the harness, so a second list
	/// cannot go quietly out of step with this one.
	static const unsigned int* PresetParamIDsForTest( int& count );

	FFResult SetTime( double time ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// `instantiateGL` pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and `CFFGLPlugin`'s
	/// `SetTextParameter` is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	/// The order the host shows them in: what is on the glass, how it moves,
	/// what the light does on the way through, what colour the dyes are, what
	/// is behind it, and then the two controls that are about the edit rather
	/// than about the wheel.
	enum ParamID : FFUInt32
	{
		//Wheel
		PT_CELLS,
		PT_SIZE,
		PT_VARIATION,
		PT_MERGE,
		PT_SPREAD,
		PT_SCATTER,
		PT_SEED,

		//Motion
		PT_SPEED,
		PT_DRIFT,
		PT_SPIN,
		PT_CHURN,
		PT_GRAIN,
		PT_BOIL,

		//Optics
		PT_DENSITY,
		PT_REFRACTION,
		PT_DISPERSION,
		PT_MENISCUS,
		PT_CAUSTIC,
		PT_RIM,

		//Colour
		PT_PALETTE,
		PT_HUE,
		PT_HUE_SPREAD,
		PT_SATURATION,

		//Lamp
		PT_LAMP,
		PT_HOTSPOT,
		PT_TEMPERATURE,
		PT_GATE,
		PT_GATE_SOFT,

		//Simmer
		PT_SIMMER,
		PT_SMEAR,

		//Output
		PT_MODE,
		PT_MIX,

		//Preset. Declared after the real controls so their IDs -- which a
		//saved composition refers to -- do not shift under existing users
		//when more presets arrive.
		PT_PRESET,

		//About. FFGL has no window and cannot make one, so the name, the
		//version, the maker and the links are parameters the host draws with
		//everything else. Last in the enum, so no saved composition's
		//parameter ids shift. See StoatworksAboutParams.h.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

	/// Fill a `flenser::Wheel` from the current parameters, for a picture of
	/// this shape. Public because the harness resolves the wheel without
	/// rendering, and because it is the single place the 0..1 space becomes
	/// physical units in this build.
	flenser::Wheel ResolveWheel( float aspect ) const;

private:
	/// The ParamID each `presets::Param` drives, in `presets::Param` order.
	/// The preset table stays host-agnostic; this is the FFGL binding of it.
	static constexpr unsigned int kPresetParamIDs[ flenser::presets::kParamCount ] = {
		PT_CELLS, PT_SIZE, PT_VARIATION, PT_MERGE, PT_SPREAD, PT_SCATTER,
		PT_SPEED, PT_DRIFT, PT_SPIN, PT_CHURN, PT_GRAIN, PT_BOIL,
		PT_DENSITY, PT_REFRACTION, PT_DISPERSION, PT_MENISCUS, PT_CAUSTIC, PT_RIM,
		PT_PALETTE, PT_HUE, PT_HUE_SPREAD, PT_SATURATION,
		PT_LAMP, PT_HOTSPOT, PT_TEMPERATURE, PT_GATE, PT_GATE_SOFT
	};

	/// The active preset's value for `id`, or -1 when no preset is active or
	/// this one has no opinion about `id`. Preset values are all 0..1, so a
	/// negative is unambiguous.
	float presetValue( int presetIndex, unsigned int id ) const;

	/// True when this write is the HOST restating a value it still believes
	/// in rather than the operator moving anything -- in which case it must
	/// not reach `params[]` and must not disturb the preset.
	bool hostIsRestatingItself( unsigned int index, float value );

	/// Record the defaults as the host's opening position, once, before
	/// anything has had a chance to move them.
	void seedHostValues();

	void applyPreset( int presetIndex );

	/// Bring the buffers to this size, reallocating only what has changed.
	/// False means the driver would not give us the memory.
	bool EnsureBuffers( GLsizei w, GLsizei h );

	/// True when this instance takes an input clip: the effect bundle.
	const bool overInput;

	/// What the HOST last sent for each parameter, which is not the same
	/// thing as what the plugin is rendering with.
	///
	/// FFGL's host owns parameter state. It pushes its own values back down
	/// whenever it likes, and nothing obliges it to act on the value events
	/// `applyPreset` raises -- Resolume does not. So a preset that writes
	/// `params[]` and trusts the host to follow is relying on behaviour the
	/// specification never promised, and when the host instead restates the
	/// values it still believes in, the rule that a covered parameter
	/// changing means the operator has taken over fires on the host's own
	/// echo and drops straight back to Custom. Reported against vertigo as
	/// its issue #2; the same pattern had been copied into seven plugins
	/// before it was found.
	///
	/// Keeping the host's own last word separately is what tells the two
	/// apart.
	float hostValues[ PT_COUNT ] = {};
	bool hostValuesSeeded        = false;

	ffglex::FFGLShader oilShader;
	ffglex::FFGLShader simmerShader;
	ffglex::FFGLShader compositeShader;
	ffglex::FFGLScreenQuad quad;

	flenser::PassBuffer oilBuffer;

	/// The feedback buffers, ping-ponged by the simmer pass. Two of them
	/// because a pass cannot read the texture it is drawing into; `feedIndex`
	/// says which one holds the current contents.
	flenser::PassBuffer feedBuffer[ 2 ];
	int feedIndex = 0;

	/// True once a feedback buffer holds something. Cleared whenever the
	/// buffers are reallocated -- drawing a cleared buffer is free, but
	/// drawing a STALE one is a frame of somebody else's footage arriving
	/// under the oil.
	bool feedPrimed = false;

	//---------------------------------------------------------------------
	// Time.
	//
	// The boil phase is accumulated from the host's clock rather than
	// computed from it. `time * boil` is the obvious form and it is wrong
	// here: moving Boil rescales the whole history, so the noise field jumps
	// to a different place the instant the control is touched -- worst at the
	// moment an operator is nudging it, which is exactly when it is being
	// watched. Integrating the rate instead means the control changes what
	// happens next and nothing else.
	//
	// Speed and Spin were the exception, and the exception was wrong. They
	// were driven by `time * speed` and `time * spin` on the reasoning that a
	// cell's orbit is a closed loop, so rescaling its phase only moves it
	// along a path it was going to travel anyway. True, and beside the point:
	// it moves it to a DIFFERENT POINT on that path, and an hour into a
	// composition a small nudge to Speed is worth hundreds of turns, so the
	// wheel teleports. Worse than the boil case, in fact, because the two
	// orbit frequencies are deliberately incommensurate -- the trajectory is
	// not a closed loop at all but a quasi-periodic one that never repeats,
	// so there is no "back where it was" to land on. Reported from a live
	// rig as "unusable in performance", which is exactly right (#1).
	//
	// Both are now ANCHORED rather than integrated: `phase + (now - clock) *
	// rate`, with the phase carried forward once per rate CHANGE instead of
	// once per frame. Same continuity, and a long session cannot accumulate
	// per-frame rounding into a drift. The anchors start at clock zero and
	// phase zero, so until the operator touches a control the arithmetic is
	// exactly the old `time * rate` -- which is what lets every rendered-frame
	// test and tools/sweep.py go on measuring what they measured before.
	//
	// The OpenFX builds and the browser demo keep `time * rate`, through
	// SetFreeRunningPhases. Both re-render on a seek and must be a pure
	// function of the timestamp; neither has an operator nudging a slider.
	//---------------------------------------------------------------------
	double hostTime     = -1.0;
	double lastHostTime = -1.0;
	double boilPhase    = 0.0;

	/// A phase advancing at a rate that may change under it, without the
	/// change moving where the picture already is.
	struct PhaseAnchor
	{
		double phase = 0.0; ///< turns accumulated up to `clock`
		double clock = 0.0; ///< host seconds at which `phase` was carried
		double rate  = 0.0; ///< turns per second since `clock`
		bool   armed = false;

		/// The phase now, carrying the anchor forward if the rate has moved.
		///
		/// `armed` rather than a sentinel rate: Spin is bipolar and Speed can
		/// be parked at zero, so every value a rate can hold is a legitimate
		/// one and none of them is free to mean "unset".
		double At( double now, double newRate )
		{
			if( !armed )
			{
				rate  = newRate;
				armed = true;
			}
			else if( newRate != rate )
			{
				phase += ( now - clock ) * rate;
				clock = now;
				rate  = newRate;
			}
			return phase + ( now - clock ) * rate;
		}
	};

	PhaseAnchor orbitAnchor;
	PhaseAnchor spinAnchor;

	//---------------------------------------------------------------------
	// Host clock units.
	//
	// The FFGL header never says what unit SetTime is in, and hosts
	// disagree: Resolume hands over MILLISECONDS (measured live: 20.0 per
	// frame at its 50 fps, and the SDK's own Particles sample divides by
	// 1000), while the offline harness -- and any host following the
	// header's silence -- sends seconds. Decided by comparing the host's
	// clock against a steady one over several frames, because the magnitude
	// of a single frame delta does not settle it.
	//---------------------------------------------------------------------
	double clockScale   = 0.0;///< 0 until decided; then 1.0 or 0.001
	double lastWallTime = -1.0;
	double wallStart    = -1.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	double lastRawTime  = -1.0;

	/// Counts frames so the sixtieth can log what the host's clock actually
	/// looks like. One line, once, in the diag log.
	int clockFrames = 0;

	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
