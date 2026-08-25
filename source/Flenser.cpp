#include "Flenser.h"

#include "Controls.h"
#include "Diag.h"
#include "Shaders.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9), so it has to be asked for by name. The symptom without it is an
//unknown-type error on ScopedFBOBinding and nothing else.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

using namespace ffglex;
using namespace flenser;

namespace
{
/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour. A logging call must never be
/// the thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

/// Seconds of host time a single frame is allowed to advance the boil by.
/// The host's clock is not ours: it jumps when the composition is scrubbed,
/// when a clip is retriggered, and by however long the machine was asleep.
constexpr double kMaxFrameDelta = 0.25;

/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

/// How much of the previous frame the simmer buffer keeps, at Simmer 1.
///
/// Strictly below 1, and that is the whole safety argument: the feedback pass
/// is a `max` against the live frame, so with a persistence under unity the
/// buffer cannot hold anything brighter than the brightest oil that has gone
/// through it. A feedback loop with a gain of 1.02 is a white screen four
/// hundred frames later, in the middle of a show.
constexpr float kSimmerPersistCeiling = 0.94f;

/// Wall clock, for hosts that never call SetTime. Steady rather than system,
/// so nothing here moves when the machine's clock is corrected.
double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}

/// Case-insensitive name order, so a list sorts the way a reader expects it
/// to rather than the way a byte comparison does.
bool nameLess( const char* a, const char* b )
{
	for( ; *a && *b; ++a, ++b )
	{
		const int ca = std::tolower( static_cast< unsigned char >( *a ) );
		const int cb = std::tolower( static_cast< unsigned char >( *b ) );
		if( ca != cb )
			return ca < cb;
	}
	return *b != '\0';
}
} // namespace

//---------------------------------------------------------------------------
FlenserPlugin::FlenserPlugin( bool overInputValue ) :
	overInput( overInputValue )
{
	SetMinInputs( overInput ? 1 : 0 );
	SetMaxInputs( overInput ? 1 : 0 );

	//The host drives the clock where it can, so that rendering the same frame
	//twice gives the same picture twice and an export matches the preview.
	//
	//Note what it cannot drive: the SIMMER buffer, which is a consequence of
	//which frames the host asked for and in what order. Everything else here
	//is a pure function of this number. See Flenser.h.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults. SetParamInfof reads each one back out of GetFloatParameter,
	// so these assignments are what the host is told the defaults are.
	//
	// They add up to a recognisable wheel doing its one thing: a couple of
	// dozen merged cells in food-colouring dyes, moving slowly, with enough
	// refraction and rim to read as liquid rather than as flat shapes. The
	// null is Mix at zero, and Simmer is off.
	//---------------------------------------------------------------------
	//Enough cells, and packed wide enough, that the wheel runs off the edges
	//of the frame. A real wheel is bigger than the gate, and a default that
	//left a tidy island of oil in the middle of a grey field would be the
	//single thing most likely to make somebody decide the plugin does not
	//look like the thing it is named after.
	params[ PT_CELLS ]     = 0.86f;//about twenty-eight
	params[ PT_SIZE ]      = 0.62f;//about a quarter of the short edge
	params[ PT_VARIATION ] = 0.40f;
	params[ PT_MERGE ]     = 0.78f;
	params[ PT_SPREAD ]    = 0.85f;
	params[ PT_SCATTER ]   = 0.30f;
	params[ PT_SEED ]      = 0.0f;//seed 1

	params[ PT_SPEED ] = 0.14f;
	params[ PT_DRIFT ] = 0.60f;
	params[ PT_SPIN ]  = 0.50f;//bipolar null: the wheel is not turning
	//High, and it has to be: Churn is what stops the cells being circles, and
	//a wheel of circles reads as a screensaver rather than as oil. It is a
	//fraction of the noise wavelength (Controls.h), so this is about a
	//quarter of one -- deformation, nowhere near folding.
	params[ PT_CHURN ] = 0.90f;
	params[ PT_GRAIN ] = 0.70f;
	params[ PT_BOIL ]  = 0.20f;

	params[ PT_DENSITY ]    = 0.85f;
	params[ PT_REFRACTION ] = 0.62f;
	params[ PT_DISPERSION ] = 0.30f;
	params[ PT_MENISCUS ]   = 0.50f;
	params[ PT_CAUSTIC ]    = 0.40f;
	params[ PT_RIM ]        = 0.38f;

	params[ PT_PALETTE ]    = static_cast< float >( Palette::Aniline );
	params[ PT_HUE ]        = 0.0f;
	params[ PT_HUE_SPREAD ] = 0.80f;
	params[ PT_SATURATION ] = 0.85f;

	params[ PT_LAMP ]        = 0.62f;//1.24: over unity, because dye eats light
	params[ PT_HOTSPOT ]     = 0.20f;
	params[ PT_TEMPERATURE ] = 0.58f;//a touch warm
	//Gate wide open by default. A round gate is the more authentic look and
	//the wrong default: an effect that arrives having cropped the frame to a
	//circle looks broken to somebody who has not read what it is, and the
	//control to undo it is not the one they will reach for.
	params[ PT_GATE ]      = 1.0f;
	params[ PT_GATE_SOFT ] = 0.20f;

	params[ PT_SIMMER ] = 0.0f;//off: see Flenser.h
	params[ PT_SMEAR ]  = 0.45f;

	params[ PT_MODE ] = static_cast< float >( LampMode::Project );
	params[ PT_MIX ]  = 1.0f;

	params[ PT_PRESET ] = 0.0f;//Custom: the sliders are the truth

	//---------------------------------------------------------------------
	// Declaration.
	//
	// Every numeric parameter is a plain 0..1 float even where it stands for
	// a count of cells or a number of turns. SetParamInfo clamps an
	// FF_TYPE_STANDARD default into 0..1 *before* a range can be attached
	// (SDK b1afaf9), so a parameter declared in cells cannot declare a
	// default in cells. The conversions live in Controls.cpp.
	//
	// Option lists are declared in alphabetical order, and every entry keeps
	// the value it has always had. Those are two separate things in FFGL:
	// SetParamElementInfo takes an element's display slot and its stored
	// value as different arguments, and the spec is explicit that picking an
	// option gives the parameter "a value equal to that of the option's
	// value" -- the slot is never stored. So a list can be re-sorted for
	// whoever has to find something in it without a saved composition, a
	// factory preset or the harness changing meaning.
	//---------------------------------------------------------------------

	auto declareOptions = [ this ]( unsigned int paramID, int count, auto nameOf ) {
		std::vector< int > order( static_cast< size_t >( count ) );
		for( int i = 0; i < count; ++i )
			order[ i ] = i;

		std::stable_sort( order.begin(), order.end(),
		                  [ & ]( int a, int b ) { return nameLess( nameOf( a ), nameOf( b ) ); } );

		for( int slot = 0; slot < count; ++slot )
			SetParamElementInfo( paramID,
			                     static_cast< unsigned int >( slot ),
			                     nameOf( order[ slot ] ),
			                     static_cast< float >( order[ slot ] ) );
	};

	SetParamInfof( PT_CELLS, "Cells", FF_TYPE_STANDARD );
	SetParamInfof( PT_SIZE, "Size", FF_TYPE_STANDARD );
	SetParamInfof( PT_VARIATION, "Variation", FF_TYPE_STANDARD );
	SetParamInfof( PT_MERGE, "Merge", FF_TYPE_STANDARD );
	SetParamInfof( PT_SPREAD, "Spread", FF_TYPE_STANDARD );
	SetParamInfof( PT_SCATTER, "Scatter", FF_TYPE_STANDARD );
	SetParamInfof( PT_SEED, "Seed", FF_TYPE_STANDARD );

	SetParamInfof( PT_SPEED, "Speed", FF_TYPE_STANDARD );
	SetParamInfof( PT_DRIFT, "Drift", FF_TYPE_STANDARD );
	SetParamInfof( PT_SPIN, "Spin", FF_TYPE_STANDARD );
	SetParamInfof( PT_CHURN, "Churn", FF_TYPE_STANDARD );
	SetParamInfof( PT_GRAIN, "Grain", FF_TYPE_STANDARD );
	SetParamInfof( PT_BOIL, "Boil", FF_TYPE_STANDARD );

	SetParamInfof( PT_DENSITY, "Density", FF_TYPE_STANDARD );
	SetParamInfof( PT_REFRACTION, "Refraction", FF_TYPE_STANDARD );
	SetParamInfof( PT_DISPERSION, "Dispersion", FF_TYPE_STANDARD );
	SetParamInfof( PT_MENISCUS, "Meniscus", FF_TYPE_STANDARD );
	SetParamInfof( PT_CAUSTIC, "Caustic", FF_TYPE_STANDARD );
	SetParamInfof( PT_RIM, "Rim", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_PALETTE, "Palette", static_cast< int >( Palette::Count ), params[ PT_PALETTE ] );
	declareOptions( PT_PALETTE, static_cast< int >( Palette::Count ),
	                []( int v ) { return PaletteName( static_cast< Palette >( v ) ); } );

	SetParamInfof( PT_HUE, "Hue", FF_TYPE_STANDARD );
	SetParamInfof( PT_HUE_SPREAD, "Hue Spread", FF_TYPE_STANDARD );
	SetParamInfof( PT_SATURATION, "Saturation", FF_TYPE_STANDARD );

	SetParamInfof( PT_LAMP, "Lamp", FF_TYPE_STANDARD );
	SetParamInfof( PT_HOTSPOT, "Hotspot", FF_TYPE_STANDARD );
	SetParamInfof( PT_TEMPERATURE, "Temperature", FF_TYPE_STANDARD );
	SetParamInfof( PT_GATE, "Gate", FF_TYPE_STANDARD );
	SetParamInfof( PT_GATE_SOFT, "Gate Soft", FF_TYPE_STANDARD );

	SetParamInfof( PT_SIMMER, "Simmer", FF_TYPE_STANDARD );
	SetParamInfof( PT_SMEAR, "Smear", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_MODE, "Mode", static_cast< int >( LampMode::Count ), params[ PT_MODE ] );
	declareOptions( PT_MODE, static_cast< int >( LampMode::Count ),
	                []( int v ) { return LampModeName( static_cast< LampMode >( v ) ); } );

	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	// Factory presets. Element 0 is Custom; picking anything else copies that
	// preset's values into the covered parameters and raises value events so
	// the host re-reads the sliders. Editing a covered slider flips back to
	// Custom.
	//
	// Custom is pinned to the top and the presets sort below it. It is not a
	// preset -- it is the statement that there isn't one -- so a list that
	// filed it between Beading and Clear Water would be lying about what it
	// is. Its value stays 0, which is what applyPreset's 1-based index and
	// the sweep's Preset context both read.
	SetOptionParamInfo( PT_PRESET, "Preset", 1 + presets::kCount, params[ PT_PRESET ] );
	{
		std::vector< int > order( static_cast< size_t >( presets::kCount ) );
		for( int i = 0; i < presets::kCount; ++i )
			order[ i ] = i + 1;
		std::stable_sort( order.begin(), order.end(), []( int a, int b ) {
			return nameLess( presets::kPresets[ a - 1 ].name, presets::kPresets[ b - 1 ].name );
		} );

		SetParamElementInfo( PT_PRESET, 0, "Custom", 0.0f );
		for( int slot = 0; slot < presets::kCount; ++slot )
			SetParamElementInfo( PT_PRESET, static_cast< unsigned int >( slot + 1 ),
			                     presets::kPresets[ order[ slot ] - 1 ].name,
			                     static_cast< float >( order[ slot ] ) );
	}

	//Thirty-three parameters is well past the point where an ungrouped list
	//in somebody else's inspector stops being readable.
	for( FFUInt32 i = PT_CELLS; i <= PT_SEED; ++i )
		SetParamGroup( i, "Wheel" );
	for( FFUInt32 i = PT_SPEED; i <= PT_BOIL; ++i )
		SetParamGroup( i, "Motion" );
	for( FFUInt32 i = PT_DENSITY; i <= PT_RIM; ++i )
		SetParamGroup( i, "Optics" );
	for( FFUInt32 i = PT_PALETTE; i <= PT_SATURATION; ++i )
		SetParamGroup( i, "Colour" );
	for( FFUInt32 i = PT_LAMP; i <= PT_GATE_SOFT; ++i )
		SetParamGroup( i, "Lamp" );
	for( FFUInt32 i = PT_SIMMER; i <= PT_SMEAR; ++i )
		SetParamGroup( i, "Simmer" );
	for( FFUInt32 i = PT_MODE; i <= PT_MIX; ++i )
		SetParamGroup( i, "Output" );
	SetParamGroup( PT_PRESET, "Preset" );

	// The About block. Declared inline rather than through a helper, because
	// SetParamInfo is protected on CFFGLPlugin and nothing outside the class
	// can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( overInput ? "Created Flenser Lamp effect" : "Created Flenser source" );

	diag::init();
}

//---------------------------------------------------------------------------
FFResult FlenserPlugin::InitGL( const FFGLViewportStruct* vp )
{
	//The base class stores this in InitGL and this override does not call it,
	//so the generator -- which has no input texture to take a size from and
	//uses the viewport instead -- would size itself from zeros.
	if( vp != nullptr )
		currentViewport = *vp;

	//The GL strings first, and unconditionally: when a shader will not
	//compile it is almost always the driver or the GL version, and knowing
	//which machine reported what is most of the diagnosis.
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	//Two of the three passes are assembled rather than written out, because
	//the oil library in the middle of them is shared verbatim with the
	//harness's probe. Held in locals so the pointers handed to Compile
	//outlive the call.
	const std::string oilSource    = OilShaderSource();
	const std::string simmerSource = SimmerShaderSource();

	struct
	{
		FFGLShader* shader;
		const char* fragment;
		const char* name;
	} const stages[] = {
		{ &oilShader, oilSource.c_str(), "oil" },
		{ &simmerShader, simmerSource.c_str(), "simmer" },
		{ &compositeShader, kCompositeShader, "composite" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( kVertexShader, stage.fragment ) )
			continue;

		//Returning FF_FAIL here is invisible to the operator: the effect
		//simply does nothing in Resolume, with no message anywhere. These two
		//lines are the only record of which pass it was -- and for the two
		//assembled shaders they are the only record at all, because the line
		//number a driver reports refers to a file that does not exist.
		diag::error( std::string( "the " ) + stage.name
		             + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Flenser: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		FFGLLog::LogToHost( "Flenser: quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
Wheel FlenserPlugin::ResolveWheel( float aspect ) const
{
	Wheel wheel;

	wheel.cells     = CellsFromParam( params[ PT_CELLS ] );
	wheel.size      = SizeFromParam( params[ PT_SIZE ] );
	wheel.variation = VariationFromParam( params[ PT_VARIATION ] );
	wheel.merge     = MergeFromParam( params[ PT_MERGE ] );
	wheel.spread    = SpreadFromParam( params[ PT_SPREAD ] );
	wheel.scatter   = ScatterFromParam( params[ PT_SCATTER ] );
	wheel.seed      = SeedFromParam( params[ PT_SEED ] );

	wheel.speed = SpeedFromParam( params[ PT_SPEED ] );
	wheel.drift = DriftFromParam( params[ PT_DRIFT ] );
	wheel.spin  = SpinFromParam( params[ PT_SPIN ] );
	wheel.churn = ChurnFromParam( params[ PT_CHURN ] );
	wheel.grain = GrainFromParam( params[ PT_GRAIN ] );
	wheel.boil  = BoilFromParam( params[ PT_BOIL ] );

	wheel.density    = DensityFromParam( params[ PT_DENSITY ] );
	wheel.refraction = RefractionFromParam( params[ PT_REFRACTION ] );
	wheel.dispersion = DispersionFromParam( params[ PT_DISPERSION ] );
	wheel.meniscus   = MeniscusFromParam( params[ PT_MENISCUS ] );
	wheel.caustic    = CausticFromParam( params[ PT_CAUSTIC ] );
	wheel.rim        = RimFromParam( params[ PT_RIM ] );

	wheel.palette    = static_cast< Palette >( PaletteFromParam( params[ PT_PALETTE ] ) );
	wheel.hue        = HueFromParam( params[ PT_HUE ] );
	wheel.hueSpread  = HueSpreadFromParam( params[ PT_HUE_SPREAD ] );
	wheel.saturation = SaturationFromParam( params[ PT_SATURATION ] );

	wheel.lamp        = LampFromParam( params[ PT_LAMP ] );
	wheel.hotspot     = HotspotFromParam( params[ PT_HOTSPOT ] );
	wheel.temperature = TemperatureFromParam( params[ PT_TEMPERATURE ] );
	wheel.gate        = GateFromParam( params[ PT_GATE ] );
	wheel.gateSoft    = GateSoftFromParam( params[ PT_GATE_SOFT ] );

	wheel.aspect = aspect;

	return wheel;
}

//---------------------------------------------------------------------------
bool FlenserPlugin::EnsureBuffers( GLsizei w, GLsizei h )
{
	const bool resized = oilBuffer.Width() != w || oilBuffer.Height() != h;

	//RGBA16F and not RGBA8. The caustic is deliberately allowed past 1 -- it
	//is light that has been concentrated, so it genuinely is brighter than
	//the lamp -- and an 8-bit intermediate would clip it flat before the Mix
	//could pull it back. It also stops the simmer buffer's repeated
	//multiplications from banding, which at 8 bits they visibly do after a
	//dozen frames.
	if( !oilBuffer.Ensure( w, h, GL_RGBA16F, PassBuffer::Sampling::Linear ) )
		return false;

	for( PassBuffer& buffer : feedBuffer )
	{
		if( !buffer.Ensure( w, h, GL_RGBA16F, PassBuffer::Sampling::Linear ) )
			return false;
	}

	if( resized )
	{
		//A reallocated buffer holds whatever texture memory the driver handed
		//back. Ensure() clears it, and this says the contents are not a
		//frame's worth of history -- drawing a cleared buffer is free, but
		//treating it as primed puts one frame of nothing under the oil.
		feedPrimed = false;
		feedIndex  = 0;
	}

	return true;
}

//---------------------------------------------------------------------------
FFResult FlenserPlugin::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	//---------------------------------------------------------------------
	// How big the picture is, and where it comes from.
	//
	// The effect takes it from the input texture. The generator has no input
	// and takes it from the viewport the host declared at InitGL -- which is
	// why this override assigns currentViewport rather than leaving it to a
	// base class it does not call.
	//---------------------------------------------------------------------
	int pictureWidth  = 0;
	int pictureHeight = 0;
	GLuint inputTexture = 0;
	float maxU = 1.0f, maxV = 1.0f;

	if( overInput )
	{
		if( pGL == nullptr || pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
			return FF_FAIL;

		const FFGLTextureStruct& picture = *pGL->inputTextures[ 0 ];
		inputTexture                     = picture.Handle;
		pictureWidth                     = static_cast< int >( picture.Width );
		pictureHeight                    = static_cast< int >( picture.Height );

		//The input texture can be bigger than the picture; MaxUV is the
		//fraction that was really drawn.
		const FFGLTexCoords coords = GetMaxGLTexCoords( picture );
		maxU                       = coords.s;
		maxV                       = coords.t;
	}
	else
	{
		pictureWidth  = static_cast< int >( currentViewport.width );
		pictureHeight = static_cast< int >( currentViewport.height );
	}

	if( pictureWidth <= 0 || pictureHeight <= 0 )
		return FF_FAIL;

	const float aspect = static_cast< float >( pictureWidth ) / static_cast< float >( pictureHeight );

	//The host's viewport, read before anything of ours changes it.
	//
	//`ScopedFBOBinding` restores the framebuffer binding and *only* the
	//framebuffer binding -- it does not touch the viewport (SDK b1afaf9,
	//FFGLScopedFBOBinding.cpp). So every pass's ResizeViewPort() leaks out
	//into the pass after it, and the composite, which draws to the host's own
	//framebuffer and so has no buffer of its own to size itself from,
	//inherits whatever the last pass left behind.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	//And the host's blend state. Nothing here blends -- every pass writes
	//full coverage -- but a host that left GL_BLEND on would have the
	//composite blended into its own framebuffer, so it is turned off and put
	//back rather than assumed.
	const GLboolean hostBlend = glIsEnabled( GL_BLEND );
	GLint hostBlendSrc = GL_ONE, hostBlendDst = GL_ZERO, hostBlendEquation = GL_FUNC_ADD;
	glGetIntegerv( GL_BLEND_SRC_RGB, &hostBlendSrc );
	glGetIntegerv( GL_BLEND_DST_RGB, &hostBlendDst );
	glGetIntegerv( GL_BLEND_EQUATION_RGB, &hostBlendEquation );

	//---------------------------------------------------------------------
	// Time.
	//
	// Normalise the host's clock to seconds first: Resolume sends
	// milliseconds, the harness sends seconds, and the header says nothing.
	// steady_clock says how much real time passed, the host says how much
	// host time passed, and the ratio names the unit outright -- 1 for
	// seconds, 1000 for milliseconds, and nothing plausible in between.
	//---------------------------------------------------------------------
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	const double raw = hostTime;

	if( clockScale == 0.0 && raw >= 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;

		// A paused host, a looping clip or a stalled frame tells us nothing.
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
		}
	}

	if( raw >= 0.0 )
		lastRawTime = raw;
	lastWallTime = wallNow;

	// Until the unit is settled -- and for a host that never calls SetTime --
	// run on the real clock: wrong in origin but right in rate, where
	// assuming seconds would be a thousand times fast on Resolume.
	const double now = ( raw >= 0.0 && clockScale != 0.0 ) ? raw * clockScale
	                                                       : wallNow - wallStart;

	//Integrate the boil rate; never rescale its history. The cell orbits are
	//NOT integrated -- see the note in Flenser.h for why the two are treated
	//differently on purpose.
	if( lastHostTime >= 0.0 )
	{
		const double delta = std::clamp( now - lastHostTime, 0.0, kMaxFrameDelta );
		boilPhase += delta * static_cast< double >( BoilFromParam( params[ PT_BOIL ] ) );
	}

	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( raw )
		            + " scale=" + std::to_string( clockScale )
		            + " seconds=" + std::to_string( now ) );

	lastHostTime = now;

	//---------------------------------------------------------------------
	// The wheel, resolved once for this frame.
	//---------------------------------------------------------------------
	Wheel wheel     = ResolveWheel( aspect );
	wheel.time      = static_cast< float >( now );
	wheel.boilPhase = static_cast< float >( boilPhase );

	//Two flat arrays rather than an array of Cell, because that is what
	//glUniform3fv wants and building them here costs a couple of dozen
	//floats a frame.
	float cellPos[ kMaxCells * 3 ] = {};
	float cellDye[ kMaxCells * 3 ] = {};
	const int cellCount            = std::clamp( wheel.cells, 0, kMaxCells );

	for( int i = 0; i < cellCount; ++i )
	{
		const Cell cell        = CellAt( i, wheel );
		cellPos[ i * 3 + 0 ]   = cell.x;
		cellPos[ i * 3 + 1 ]   = cell.y;
		cellPos[ i * 3 + 2 ]   = cell.radius;
		cellDye[ i * 3 + 0 ]   = cell.dye[ 0 ];
		cellDye[ i * 3 + 1 ]   = cell.dye[ 1 ];
		cellDye[ i * 3 + 2 ]   = cell.dye[ 2 ];
	}

	const float simmer = SimmerFromParam( params[ PT_SIMMER ] );
	const float smear  = SmearFromParam( params[ PT_SMEAR ] );
	const int mode     = ModeFromParam( params[ PT_MODE ] );

	//---------------------------------------------------------------------
	// Buffers.
	//
	// Every Ensure() happens here, before anything binds a texture. That is
	// not tidiness: ffglex::FFGLFBO::Initialise sizes its new colour texture
	// under a ScopedTextureBinding, and every ffglex Scoped* binding *clears*
	// to 0 on scope exit rather than restoring what was there. Allocating a
	// buffer therefore unbinds the input texture from the active unit, and
	// the symptom is the dangerous part -- correct on every frame except the
	// one that allocates, so a control reads as dead for a single frame after
	// load and once more each time a canvas resize reallocates.
	//---------------------------------------------------------------------
	if( !EnsureBuffers( pictureWidth, pictureHeight ) )
	{
		diag::error( "could not allocate the render buffers at "
		             + std::to_string( pictureWidth ) + "x" + std::to_string( pictureHeight ) );
		return FF_FAIL;
	}

	glDisable( GL_BLEND );

	//---------------------------------------------------------------------
	// 1. The oil. Everything expensive, once, into a buffer of ours.
	//---------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( oilBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		oilBuffer.ResizeViewPort();
		ScopedShaderBinding shader( oilShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( inputTexture );

		oilShader.Set( "InputTexture", 0 );
		oilShader.Set( "MaxUV", maxU, maxV );
		oilShader.Set( "HalfTexel",
		               0.5f / static_cast< float >( pictureWidth ),
		               0.5f / static_cast< float >( pictureHeight ) );

		oilShader.Set( "Aspect", aspect );

		//The uniform ARRAYS, which FFGLShader::Set has no overload for -- its
		//four are float, vec2, vec3, vec4 and int, and nothing else. Asking
		//it for one silently issues the wrong glUniform against the wrong
		//type, which is a GL_INVALID_OPERATION that leaves the uniform at
		//zero with nothing anywhere the plugin can see.
		oilShader.Set( "CellCount", cellCount );
		if( cellCount > 0 )
		{
			glUniform3fv( oilShader.FindUniform( "CellPos" ), cellCount, cellPos );
			glUniform3fv( oilShader.FindUniform( "CellDye" ), cellCount, cellDye );
		}

		oilShader.Set( "Merge", wheel.merge );
		oilShader.Set( "Density", wheel.density );
		oilShader.Set( "Rim", wheel.rim );
		oilShader.Set( "Churn", wheel.churn );
		oilShader.Set( "Grain", wheel.grain );
		oilShader.Set( "BoilPhase", wheel.boilPhase );

		oilShader.Set( "Refraction", wheel.refraction );
		oilShader.Set( "Dispersion", wheel.dispersion );
		oilShader.Set( "Meniscus", wheel.meniscus );
		oilShader.Set( "Caustic", wheel.caustic );

		oilShader.Set( "Lamp", wheel.lamp );
		oilShader.Set( "Hotspot", wheel.hotspot );
		oilShader.Set( "Temperature", wheel.temperature );
		oilShader.Set( "Gate", wheel.gate );
		oilShader.Set( "GateSoft", wheel.gateSoft );

		oilShader.Set( "Mode", mode );
		oilShader.Set( "HasClip", overInput ? 1 : 0 );

		quad.Draw();
	}

	//---------------------------------------------------------------------
	// 2. Simmer. Skipped entirely when it is off, which is the default.
	//---------------------------------------------------------------------
	if( simmer > 0.0f )
	{
		const int from = feedIndex;
		const int to   = 1 - feedIndex;

		ScopedFBOBinding fbo( feedBuffer[ to ].GetGLID(), ScopedFBOBinding::RB_REVERT );
		feedBuffer[ to ].ResizeViewPort();
		ScopedShaderBinding shader( simmerShader.GetGLID() );

		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding oilTexture( oilBuffer.TextureID() );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding feedTexture( feedBuffer[ from ].TextureID() );

		simmerShader.Set( "OilTexture", 0 );
		simmerShader.Set( "FeedTexture", 1 );
		simmerShader.Set( "Aspect", aspect );
		simmerShader.Set( "Churn", wheel.churn );
		simmerShader.Set( "Grain", wheel.grain );
		simmerShader.Set( "BoilPhase", wheel.boilPhase );
		simmerShader.Set( "Smear", smear );

		//A buffer that has never been written holds nothing, and reading it
		//as history would put one frame of black under the oil on the frame
		//the control is switched on. Zero persistence on that one frame makes
		//the pass a straight copy of the live oil.
		simmerShader.Set( "Persist", feedPrimed ? kSimmerPersistCeiling * simmer : 0.0f );

		//CellCount is declared by the shared library and is not used by this
		//pass. Set anyway: an unset int sampler-adjacent uniform is zero,
		//which is the right answer here, but relying on that would break the
		//moment the library grows a use for it.
		simmerShader.Set( "CellCount", 0 );

		quad.Draw();

		feedIndex  = to;
		feedPrimed = true;
	}

	//---------------------------------------------------------------------
	// 3. Composite, straight to the host's framebuffer.
	//---------------------------------------------------------------------
	{
		//Back to the host's viewport. See the note where it was captured.
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( compositeShader.GetGLID() );

		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding oilTexture( oilBuffer.TextureID() );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding feedTexture( feedBuffer[ feedIndex ].TextureID() );
		ScopedSamplerActivation sampler2( 2 );
		//For the generator there is no clip; the oil buffer is bound so that
		//the sampler has something valid attached, and HasClip stops it being
		//read. Leaving a sampler bound to nothing in a core profile is not a
		//defined no-op.
		Scoped2DTextureBinding sourceTexture( overInput ? inputTexture : oilBuffer.TextureID() );

		compositeShader.Set( "OilTexture", 0 );
		compositeShader.Set( "FeedTexture", 1 );
		compositeShader.Set( "SourceTexture", 2 );
		compositeShader.Set( "SourceMaxUV", maxU, maxV );

		compositeShader.Set( "Simmer", simmer );
		compositeShader.Set( "MixAmount", params[ PT_MIX ] );
		compositeShader.Set( "HasClip", overInput ? 1 : 0 );

		quad.Draw();
	}

	//Put the host's blend state back exactly as it was found.
	glBlendEquation( hostBlendEquation );
	glBlendFunc( hostBlendSrc, hostBlendDst );
	if( hostBlend == GL_TRUE )
		glEnable( GL_BLEND );
	else
		glDisable( GL_BLEND );

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult FlenserPlugin::DeInitGL()
{
	oilShader.FreeGLResources();
	simmerShader.FreeGLResources();
	compositeShader.FreeGLResources();
	quad.Release();

	oilBuffer.Destroy();
	feedBuffer[ 0 ].Destroy();
	feedBuffer[ 1 ].Destroy();

	feedIndex  = 0;
	feedPrimed = false;

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult FlenserPlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	seedHostValues();

	// The About buttons open a browser and store nothing, so they are handled
	// before the params[] write below -- there is no value to keep.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	if( index == PT_PRESET )
	{
		const int chosen = static_cast< int >( std::lround( value ) );
		if( chosen != static_cast< int >( std::lround( params[ PT_PRESET ] ) ) )
			applyPreset( chosen );
		return FF_SUCCESS;
	}

	// The host may be restating a value it still believes in rather than the
	// operator moving anything. Letting that through would overwrite the
	// preset's value in params[] AND read as an edit, dropping the dropdown
	// back to Custom -- which is what made presets look like they could not
	// be selected at all elsewhere in the fleet. See AGENTS.md.
	if( hostIsRestatingItself( index, value ) )
		return FF_SUCCESS;

	const float previous = params[ index ];
	params[ index ]      = value;

	// A slider moved while a preset is active means the operator has taken
	// over: the dropdown falls back to Custom. The equality guard matters --
	// hosts that honour the value events echo the preset's own values
	// straight back through here, and that echo must not un-set the preset.
	const int active = static_cast< int >( std::lround( params[ PT_PRESET ] ) );
	if( active > 0 && std::fabs( value - previous ) > 1e-4f )
	{
		for( unsigned int id : kPresetParamIDs )
		{
			if( id == index )
			{
				// Logged, unlike an ordinary parameter change: this one is a
				// state change an operator can be surprised by, it happens
				// once rather than per frame, and diagnosing the same defect
				// in vertigo needed a code read precisely because nothing
				// said it had happened.
				diag::info( "preset dropped to Custom: parameter "
				            + std::to_string( index ) + " moved to "
				            + std::to_string( value ) );
				params[ PT_PRESET ] = 0.0f;
				RaiseParamEvent( PT_PRESET, FF_EVENT_FLAG_VALUE );
				break;
			}
		}
	}

	return FF_SUCCESS;
}

const unsigned int* FlenserPlugin::PresetParamIDsForTest( int& count )
{
	count = flenser::presets::kParamCount;
	return kPresetParamIDs;
}

float FlenserPlugin::presetValue( int presetIndex, unsigned int id ) const
{
	if( presetIndex <= 0 || presetIndex > flenser::presets::kCount )
		return -1.0f;

	const flenser::presets::Preset& preset = flenser::presets::kPresets[ presetIndex - 1 ];
	for( int j = 0; j < flenser::presets::kParamCount; ++j )
		if( kPresetParamIDs[ j ] == id )
			return preset.v[ j ];

	return -1.0f;
}

void FlenserPlugin::seedHostValues()
{
	// Seeded on first parameter traffic rather than in the constructor, so
	// the whole mechanism stays in one place. It has to happen BEFORE
	// applyPreset can run: seeding afterwards would record the preset's own
	// values as the host's opening position, and the host's very next
	// restatement would then look like an edit -- which is the bug this
	// exists to fix, reintroduced.
	if( hostValuesSeeded )
		return;

	for( unsigned int i = 0; i < PT_COUNT; ++i )
		hostValues[ i ] = params[ i ];
	hostValuesSeeded = true;
}

bool FlenserPlugin::hostIsRestatingItself( unsigned int index, float value )
{
	const float lastFromHost = hostValues[ index ];
	hostValues[ index ]      = value;

	const float fromPreset =
		presetValue( static_cast< int >( std::lround( params[ PT_PRESET ] ) ), index );
	if( fromPreset < 0.0f )
		return false;

	// A quantisation allowance rather than a float epsilon. A host that keeps
	// its parameters shorter than a float -- or round-trips them through a
	// UI, a MIDI value or a saved composition -- hands back a number near
	// ours rather than ours, and 1e-4 read that as an edit.
	constexpr float kSame = 1e-3f;

	if( std::fabs( value - fromPreset ) <= kSame )
	{
		// The host agreeing with the preset. Nothing to write -- and writing
		// it would actively hurt: a host that quantises hands back a ROUNDED
		// copy of our own value, params[] would take the rounding, and the
		// "did a covered parameter move?" test above works to a tighter
		// tolerance than this one and would read that rounding as an edit.
		return true;
	}

	if( std::fabs( value - lastFromHost ) > kSame )
		return false;//neither: the operator has taken over

	// Deliberately not logged. A host that pushes its parameters every frame
	// would put a line here every frame, and a log that scrolls is a log
	// nobody reads.
	return true;
}

void FlenserPlugin::applyPreset( int presetIndex )
{
	params[ PT_PRESET ] = static_cast< float >( presetIndex );

	if( presetIndex <= 0 || presetIndex > presets::kCount )
		return;//Custom: the sliders keep whatever they said

	const presets::Preset& preset = presets::kPresets[ presetIndex - 1 ];
	for( int j = 0; j < presets::kParamCount; ++j )
	{
		const unsigned int id = kPresetParamIDs[ j ];
		if( std::fabs( params[ id ] - preset.v[ j ] ) <= 1e-6f )
			continue;

		// The copy is what changes the picture; the event only tells the host
		// to re-read the slider. A host that ignores it renders the preset
		// correctly and merely shows stale knobs.
		params[ id ] = preset.v[ j ];
		RaiseParamEvent( id, FF_EVENT_FLAG_VALUE );
	}
}

float FlenserPlugin::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

//---------------------------------------------------------------------------
char* FlenserPlugin::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

//---------------------------------------------------------------------------
FFResult FlenserPlugin::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only, so there is genuinely
	// nothing to store -- but it has to say so successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult FlenserPlugin::SetTime( double time )
{
	hostTime = time;
	return FF_SUCCESS;
}

void FlenserPlugin::SetClockScaleForTest( double scale )
{
	clockScale = scale;
}
