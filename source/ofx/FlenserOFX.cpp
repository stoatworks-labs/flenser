/// The OpenFX builds of Flenser, for DaVinci Resolve, Nuke, Natron, Vegas and
/// other OFX hosts. Two plugins from this one file, as the FFGL side ships two
/// bundles: "Flenser" is a generator -- the wheel with its own lamp behind it
/// -- and "Flenser Lamp" puts the incoming clip where the lamp was.
///
/// The wheel still has exactly one home: `Oil.cpp`, the same C++ the FFGL
/// build solves per cell and `fltest` measures. What this file mirrors from
/// `Shaders.cpp` is the *composite* -- the order the dye stack, the meniscus,
/// the caustic and the gate are applied in, which the GPU did per fragment.
/// Everything below that is a call into the shared code. When editing the oil
/// shader's main, edit this too.
///
/// **Simmer does not exist here, and that is not an omission to fill in
/// later.** OpenFX hosts render frames in any order, alone, on several threads
/// at once; Resolve rendering frame 500 before frame 4 is normal. A feedback
/// buffer in that host either serialises it or renders differently every time,
/// and an effect that does not match its own preview on the second export is
/// worse than an effect that is missing a control. The plugin description says
/// so, so that somebody comparing the two builds finds out from the plugin
/// rather than from the picture.
///
/// The other departure: OFX hands render time in *frames*, and the boil phase
/// is derived as `seconds * Boil` rather than integrated. The FFGL build
/// integrates so that nudging Boil live does not rescale the field's whole
/// history -- there is no previous frame to have integrated from here, and a
/// deterministic frame matters more in a host that renders them out of order.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsProcessing.h"

// After the OFX Support headers, which is where the OFX types come from.
#include "StoatworksAboutOFX.h"

#include "../Controls.h"
#include "../Oil.h"
#include "../Presets.h"

namespace
{
constexpr const char* kSourceIdentifier = "com.stoatworks.flenser";
constexpr const char* kLampIdentifier   = "com.stoatworks.flenserlamp";
constexpr const char* kPluginGrouping   = "Stoatworks";

constexpr const char* kPluginDescription =
	"A liquid light show: oil, water, alcohol and dye between two watch "
	"glasses on an overhead projector.\n\n"
	"Cells of oil press on each other and join with a fillet. Each one is a "
	"dye filter, so overlapping cells MULTIPLY -- cyan over magenta is blue, "
	"not white. The meniscus at every edge is a lens: it displaces what is "
	"behind it, splits it by wavelength, and throws a bright caustic just "
	"inside a dark rim. Behind all of it is a condenser with a hot spot, a "
	"colour temperature and a round gate.\n\n"
	"Every cell's position is a pure function of time, so any frame renders "
	"on its own and scrubbing shows the wheel at that moment.\n\n"
	"Note: the Simmer control in the Resolume build is not here. It is a "
	"feedback buffer, and this host renders frames out of order.\n\n"
	"https://stoatworks-labs.com";

constexpr const char* kParamCells      = "cells";
constexpr const char* kParamSize       = "size";
constexpr const char* kParamVariation  = "variation";
constexpr const char* kParamMerge      = "merge";
constexpr const char* kParamSpread     = "spread";
constexpr const char* kParamScatter    = "scatter";
constexpr const char* kParamSeed       = "seed";
constexpr const char* kParamSpeed      = "speed";
constexpr const char* kParamDrift      = "drift";
constexpr const char* kParamSpin       = "spin";
constexpr const char* kParamChurn      = "churn";
constexpr const char* kParamGrain      = "grain";
constexpr const char* kParamBoil       = "boil";
constexpr const char* kParamDensity    = "density";
constexpr const char* kParamRefraction = "refraction";
constexpr const char* kParamDispersion = "dispersion";
constexpr const char* kParamMeniscus   = "meniscus";
constexpr const char* kParamCaustic    = "caustic";
constexpr const char* kParamRim        = "rim";
constexpr const char* kParamPalette    = "palette";
constexpr const char* kParamHue        = "hue";
constexpr const char* kParamHueSpread  = "hueSpread";
constexpr const char* kParamSaturation = "saturation";
constexpr const char* kParamLamp       = "lamp";
constexpr const char* kParamHotspot    = "hotspot";
constexpr const char* kParamTemp       = "temperature";
constexpr const char* kParamGate       = "gate";
constexpr const char* kParamGateSoft   = "gateSoft";
constexpr const char* kParamMode       = "mode";
constexpr const char* kParamMix        = "mix";
constexpr const char* kParamPreset     = "preset";

using namespace flenser;

//---------------------------------------------------------------------------
/// Everything one render needs, snapshotted before a pixel is touched.
///
/// OFX forbids reading a parameter during render -- and even where a host
/// tolerates it, the parameter would be read once per pixel per thread. So
/// the whole frame's worth of state is resolved into this, once, on the
/// calling thread.
//---------------------------------------------------------------------------
struct OilSetup
{
	Wheel wheel;
	std::vector< Cell > cells;

	LampMode mode = LampMode::Project;
	float mix     = 1.0f;

	bool hasClip       = false;
	bool premultiplied = false;
};

//---------------------------------------------------------------------------
// The composite, mirrored from the oil shader's main. Written once here and
// used by every pixel depth.
//---------------------------------------------------------------------------

/// Rec.709 luma. Used for how bright a caustic is and for what the clip
/// contributes in Colourise, and both want the same answer.
inline float luma( float r, float g, float b )
{
	return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

inline float clamp01( float v )
{
	return std::min( std::max( v, 0.0f ), 1.0f );
}

class FlenserProcessorBase : public OFX::ImageProcessor
{
public:
	explicit FlenserProcessorBase( OFX::ImageEffect& effect ) :
		OFX::ImageProcessor( effect )
	{
	}

	void setSetup( OFX::Image* src, const OilSetup* v )
	{
		srcImg = src;
		setup  = v;
	}

protected:
	OFX::Image* srcImg   = nullptr;
	const OilSetup* setup = nullptr;
};

template< class PIX, int nComponents, int maxValue >
class FlenserProcessor : public FlenserProcessorBase
{
public:
	explicit FlenserProcessor( OFX::ImageEffect& effect ) :
		FlenserProcessorBase( effect )
	{
	}

	void multiThreadProcessImages( OfxRectI window ) override
	{
		const OilSetup& s     = *setup;
		const OfxRectI bounds = _dstImg->getBounds();
		const int outW        = bounds.x2 - bounds.x1;
		const int outH        = bounds.y2 - bounds.y1;

		if( outW <= 0 || outH <= 0 )
			return;

		const float aspect = s.wheel.aspect;
		const int cellCount = static_cast< int >( s.cells.size() );

		//Dispersion: red and blue bent by different amounts, green left alone
		//so the picture does not appear to move as the control opens.
		const float dr = 1.0f + 0.30f * s.wheel.dispersion;
		const float db = 1.0f - 0.30f * s.wheel.dispersion;

		for( int y = window.y1; y < window.y2; ++y )
		{
			if( _effect.abort() )
				break;

			PIX* dstPix = static_cast< PIX* >( _dstImg->getPixelAddress( window.x1, y ) );

			//Frame space runs y-DOWN, to match the uv convention on the GLSL
			//side; OFX rows run bottom-up. Getting this backwards mirrors the
			//whole wheel, which on a symmetric arrangement is invisible until
			//somebody compares the two builds frame for frame.
			const float v = 1.0f - ( static_cast< float >( y - bounds.y1 ) + 0.5f ) / outH;

			for( int x = window.x1; x < window.x2; ++x, dstPix += nComponents )
			{
				const float u = ( static_cast< float >( x - bounds.x1 ) + 0.5f ) / outW;

				float px = 0.0f, py = 0.0f;
				UvToWheel( u, v, aspect, px, py );

				//The churn moves the OIL. The gate does not move with it: the
				//gate is a hole in the projector's casting and the oil is on
				//the glass above it.
				float wx = px, wy = py;
				WarpPoint( px, py, s.wheel.churn, s.wheel.grain, s.wheel.boilPhase, wx, wy );

				const Sample field = FieldAt( wx, wy, s.cells.data(), cellCount,
				                              s.wheel.merge, s.wheel.density, s.wheel.rim );

				float bendX = 0.0f, bendY = 0.0f;
				BendAt( field, s.wheel.refraction, s.wheel.rim, bendX, bendY );

				const float gate = GateAt( px, py, s.wheel.gate, s.wheel.gateSoft );

				float cst = 0.0f, men = 0.0f;
				RimProfiles( field.dn, s.wheel.rim, cst, men );

				//Wheel units into picture units. The short edge is 2 wheel
				//units, so a displacement of 0.02 is one per cent of the
				//picture height whatever the aspect ratio is.
				const float bendU = bendX / ( 2.0f * aspect );
				const float bendV = bendY * 0.5f;

				float clip[ 4 ]  = { 0.0f, 0.0f, 0.0f, 0.0f };
				float light[ 3 ] = { 0.0f, 0.0f, 0.0f };

				if( s.hasClip )
					sampleClip( u, v, bounds, outW, outH, clip );

				if( s.hasClip && s.mode == LampMode::Project )
				{
					float r[ 4 ], g[ 4 ], b[ 4 ];
					sampleClip( u - bendU * dr, v - bendV * dr, bounds, outW, outH, r );
					sampleClip( u - bendU, v - bendV, bounds, outW, outH, g );
					sampleClip( u - bendU * db, v - bendV * db, bounds, outW, outH, b );

					float lr, lg, lb;
					SynthLamp( px, py, s.wheel.lamp, s.wheel.hotspot, s.wheel.temperature, lr, lg, lb );

					light[ 0 ] = r[ 0 ] * lr;
					light[ 1 ] = g[ 1 ] * lg;
					light[ 2 ] = b[ 2 ] * lb;
				}
				else if( s.hasClip && s.mode == LampMode::Colourise )
				{
					float c[ 4 ];
					sampleClip( u - bendU, v - bendV, bounds, outW, outH, c );
					const float lit = luma( c[ 0 ], c[ 1 ], c[ 2 ] );

					float lr, lg, lb;
					SynthLamp( px, py, s.wheel.lamp, s.wheel.hotspot, s.wheel.temperature, lr, lg, lb );

					light[ 0 ] = lit * lr;
					light[ 1 ] = lit * lg;
					light[ 2 ] = lit * lb;
				}
				else
				{
					//The generator, and Over: the plugin's own lamp, bent by
					//the same meniscus. Sampling an analytic lamp at a
					//displaced point costs nothing, so it gets the same
					//three-channel dispersion the clip does.
					float r[ 3 ], g[ 3 ], b[ 3 ];
					SynthLamp( px - bendX * dr, py - bendY * dr, s.wheel.lamp, s.wheel.hotspot,
					           s.wheel.temperature, r[ 0 ], r[ 1 ], r[ 2 ] );
					SynthLamp( px - bendX, py - bendY, s.wheel.lamp, s.wheel.hotspot,
					           s.wheel.temperature, g[ 0 ], g[ 1 ], g[ 2 ] );
					SynthLamp( px - bendX * db, py - bendY * db, s.wheel.lamp, s.wheel.hotspot,
					           s.wheel.temperature, b[ 0 ], b[ 1 ], b[ 2 ] );

					light[ 0 ] = r[ 0 ];
					light[ 1 ] = g[ 1 ];
					light[ 2 ] = b[ 2 ];
				}

				//---- the dye stack -------------------------------------
				float col[ 3 ];
				for( int c = 0; c < 3; ++c )
					col[ c ] = light[ c ] * field.t[ c ];

				//The meniscus line: light hitting a steep boundary is thrown
				//sideways rather than forward.
				const float dark = 1.0f - s.wheel.meniscus * men;
				for( int c = 0; c < 3; ++c )
					col[ c ] *= dark;

				//The caustic. ADDED, because it is light -- concentrated by
				//the same curvature that darkened the rim. Tinted halfway
				//towards the local dye: a caustic thrown through a red cell
				//arrives red, but it has been through less dye than the
				//middle of the cell has.
				const float causticAmount =
					s.wheel.caustic * cst * luma( light[ 0 ], light[ 1 ], light[ 2 ] );
				for( int c = 0; c < 3; ++c )
					col[ c ] = std::max( col[ c ] + causticAmount * ( 0.5f + 0.5f * field.t[ c ] ), 0.0f );

				//---- out through the gate ------------------------------
				float out[ 4 ];
				if( s.hasClip && s.mode == LampMode::Over )
				{
					const float dyeCover = 1.0f - luma( field.t[ 0 ], field.t[ 1 ], field.t[ 2 ] );
					const float rimCover =
						std::max( s.wheel.meniscus * men, std::min( s.wheel.caustic * cst, 1.0f ) );
					const float cover = clamp01( std::max( dyeCover, rimCover ) ) * gate;

					for( int c = 0; c < 3; ++c )
						out[ c ] = clip[ c ] * ( 1.0f - cover ) + col[ c ] * cover;
					out[ 3 ] = clamp01( clip[ 3 ] * ( 1.0f - cover ) + cover );
				}
				else
				{
					for( int c = 0; c < 3; ++c )
						out[ c ] = col[ c ] * gate;
					out[ 3 ] = clamp01( s.hasClip ? clip[ 3 ] * gate : gate );
				}

				//---- Mix, against the untouched clip -------------------
				if( s.hasClip && s.mix < 1.0f )
				{
					for( int c = 0; c < 4; ++c )
						out[ c ] = clip[ c ] * ( 1.0f - s.mix ) + out[ c ] * s.mix;
				}
				else if( !s.hasClip && s.mix < 1.0f )
				{
					//The generator mixes against nothing, which is a fade to
					//transparent rather than to black -- premultiplied, so
					//every channel scales together.
					for( int c = 0; c < 4; ++c )
						out[ c ] *= s.mix;
				}

				for( int c = 0; c < nComponents; ++c )
				{
					const float value = c < 4 ? out[ c ] : 0.0f;
					dstPix[ c ]       = fromFloat( value );
				}
			}
		}
	}

private:
	static PIX fromFloat( float value )
	{
		//Float targets are NOT clamped at 1. The caustic is deliberately
		//allowed past unity -- it is light that has been concentrated -- and
		//a float pipeline in Resolve is exactly where an operator would want
		//that headroom preserved for a grade downstream.
		if( maxValue == 1 )
			return static_cast< PIX >( std::max( value, 0.0f ) );

		const float scaled = clamp01( value ) * static_cast< float >( maxValue );
		return static_cast< PIX >( scaled + 0.5f );
	}

	static float toFloat( PIX value )
	{
		return maxValue == 1 ? static_cast< float >( value )
		                     : static_cast< float >( value ) / static_cast< float >( maxValue );
	}

	/// Bilinear fetch of the source clip at a point in frame space, clamped to
	/// the edge.
	///
	/// Clamped and not wrapped: the refraction at the rim of a cell that
	/// overhangs the frame deliberately reads off the side of the picture, and
	/// wrapped it would bring the opposite edge in with it -- which does not
	/// read as an error, it reads as a second copy of the clip inside the oil.
	void sampleClip( float u, float v, const OfxRectI& bounds, int outW, int outH,
	                 float out[ 4 ] ) const
	{
		out[ 0 ] = out[ 1 ] = out[ 2 ] = out[ 3 ] = 0.0f;
		if( srcImg == nullptr )
			return;

		const OfxRectI src = srcImg->getBounds();
		const int srcW     = src.x2 - src.x1;
		const int srcH     = src.y2 - src.y1;
		if( srcW <= 0 || srcH <= 0 )
			return;

		//Frame space is y-down; OFX rows are bottom-up.
		const float fx = u * srcW - 0.5f;
		const float fy = ( 1.0f - v ) * srcH - 0.5f;

		const int x0 = static_cast< int >( std::floor( fx ) );
		const int y0 = static_cast< int >( std::floor( fy ) );
		const float tx = fx - static_cast< float >( x0 );
		const float ty = fy - static_cast< float >( y0 );

		float acc[ 4 ] = { 0.0f, 0.0f, 0.0f, 0.0f };
		for( int j = 0; j < 2; ++j )
		{
			for( int i = 0; i < 2; ++i )
			{
				const int sx = std::min( std::max( x0 + i, 0 ), srcW - 1 ) + src.x1;
				const int sy = std::min( std::max( y0 + j, 0 ), srcH - 1 ) + src.y1;

				const PIX* pixel = static_cast< const PIX* >( srcImg->getPixelAddress( sx, sy ) );
				if( pixel == nullptr )
					continue;

				const float weight = ( i ? tx : 1.0f - tx ) * ( j ? ty : 1.0f - ty );
				for( int c = 0; c < 4; ++c )
					acc[ c ] += weight * ( c < nComponents ? toFloat( pixel[ c ] ) : ( c == 3 ? 1.0f : 0.0f ) );
			}
		}

		(void)bounds;
		(void)outW;
		(void)outH;
		std::memcpy( out, acc, sizeof( acc ) );
	}
};

//---------------------------------------------------------------------------
class FlenserOFXPlugin : public OFX::ImageEffect
{
public:
	FlenserOFXPlugin( OfxImageEffectHandle handle, bool overInputValue ) :
		OFX::ImageEffect( handle ),
		overInput( overInputValue )
	{
		dstClip = fetchClip( kOfxImageEffectOutputClipName );
		if( overInput )
			srcClip = fetchClip( kOfxImageEffectSimpleSourceClipName );

		cells      = fetchDoubleParam( kParamCells );
		size       = fetchDoubleParam( kParamSize );
		variation  = fetchDoubleParam( kParamVariation );
		merge      = fetchDoubleParam( kParamMerge );
		spread     = fetchDoubleParam( kParamSpread );
		scatter    = fetchDoubleParam( kParamScatter );
		seed       = fetchDoubleParam( kParamSeed );
		speed      = fetchDoubleParam( kParamSpeed );
		drift      = fetchDoubleParam( kParamDrift );
		spin       = fetchDoubleParam( kParamSpin );
		churn      = fetchDoubleParam( kParamChurn );
		grain      = fetchDoubleParam( kParamGrain );
		boil       = fetchDoubleParam( kParamBoil );
		density    = fetchDoubleParam( kParamDensity );
		refraction = fetchDoubleParam( kParamRefraction );
		dispersion = fetchDoubleParam( kParamDispersion );
		meniscus   = fetchDoubleParam( kParamMeniscus );
		caustic    = fetchDoubleParam( kParamCaustic );
		rim        = fetchDoubleParam( kParamRim );
		palette    = fetchChoiceParam( kParamPalette );
		hue        = fetchDoubleParam( kParamHue );
		hueSpread  = fetchDoubleParam( kParamHueSpread );
		saturation = fetchDoubleParam( kParamSaturation );
		lamp       = fetchDoubleParam( kParamLamp );
		hotspot    = fetchDoubleParam( kParamHotspot );
		temperature = fetchDoubleParam( kParamTemp );
		gate       = fetchDoubleParam( kParamGate );
		gateSoft   = fetchDoubleParam( kParamGateSoft );
		if( overInput )
		{
			mode = fetchChoiceParam( kParamMode );
			mix  = fetchDoubleParam( kParamMix );
		}
		preset = fetchChoiceParam( kParamPreset );
	}

	void changedParam( const OFX::InstanceChangedArgs& args, const std::string& paramName ) override
	{
		// The About links open a browser and change nothing about the render.
		if( stoatworks::about::ofx::changedParam( args, paramName ) )
			return;

		using namespace flenser::presets;

		if( paramName == kParamPreset )
		{
			int chosen = 0;
			preset->getValue( chosen );
			if( chosen <= 0 || chosen > kCount || applyingPreset )
				return;

			// The copy IS the preset -- same table as the FFGL build, same
			// 0..1 space. One edit block so undo takes the whole preset back
			// at once.
			const Preset& p = kPresets[ chosen - 1 ];
			applyingPreset  = true;
			beginEditBlock( "Preset" );
			for( int j = 0; j < kParamCount; ++j )
				setCovered( j, p.v[ j ] );
			endEditBlock();
			applyingPreset = false;
			return;
		}

		// Editing a covered control while a preset is active hands control
		// back to the sliders. Judged by VALUE, not by the change reason:
		// hosts are not consistent about reasons, but "still equal to the
		// preset" is unambiguous and also absorbs the host echoing our own
		// setValues.
		if( applyingPreset || args.reason == OFX::eChangeTime )
			return;

		int active = 0;
		preset->getValue( active );
		if( active <= 0 || active > kCount )
			return;

		const Preset& p = kPresets[ active - 1 ];
		for( int j = 0; j < kParamCount; ++j )
		{
			if( paramName != coveredName( j ) || !coveredDiffers( j, p.v[ j ] ) )
				continue;

			applyingPreset = true;
			preset->setValue( 0 );
			applyingPreset = false;
			return;
		}
	}

	void render( const OFX::RenderArguments& args ) override
	{
		std::unique_ptr< OFX::Image > dst( dstClip->fetchImage( args.time ) );
		std::unique_ptr< OFX::Image > src;
		if( overInput && srcClip != nullptr && srcClip->isConnected() )
			src.reset( srcClip->fetchImage( args.time ) );

		const OFX::BitDepthEnum depth       = dst->getPixelDepth();
		const OFX::PixelComponentEnum comps = dst->getPixelComponents();

		if( comps != OFX::ePixelComponentRGBA && comps != OFX::ePixelComponentRGB )
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );

		OilSetup setup;
		buildSetup( args, *dst, src.get(), setup );

		switch( depth )
		{
		case OFX::eBitDepthUByte:
			comps == OFX::ePixelComponentRGBA
				? run< FlenserProcessor< unsigned char, 4, 255 > >( args, dst.get(), src.get(), setup )
				: run< FlenserProcessor< unsigned char, 3, 255 > >( args, dst.get(), src.get(), setup );
			break;
		case OFX::eBitDepthUShort:
			comps == OFX::ePixelComponentRGBA
				? run< FlenserProcessor< unsigned short, 4, 65535 > >( args, dst.get(), src.get(), setup )
				: run< FlenserProcessor< unsigned short, 3, 65535 > >( args, dst.get(), src.get(), setup );
			break;
		case OFX::eBitDepthFloat:
			comps == OFX::ePixelComponentRGBA
				? run< FlenserProcessor< float, 4, 1 > >( args, dst.get(), src.get(), setup )
				: run< FlenserProcessor< float, 3, 1 > >( args, dst.get(), src.get(), setup );
			break;
		default:
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		}
	}

private:
	void buildSetup( const OFX::RenderArguments& args, OFX::Image& dst, OFX::Image* src,
	                 OilSetup& setup )
	{
		const double t = args.time;

		Wheel& w = setup.wheel;

		w.cells     = CellsFromParam( at( cells, t ) );
		w.size      = SizeFromParam( at( size, t ) );
		w.variation = VariationFromParam( at( variation, t ) );
		w.merge     = MergeFromParam( at( merge, t ) );
		w.spread    = SpreadFromParam( at( spread, t ) );
		w.scatter   = ScatterFromParam( at( scatter, t ) );
		w.seed      = SeedFromParam( at( seed, t ) );

		w.speed = SpeedFromParam( at( speed, t ) );
		w.drift = DriftFromParam( at( drift, t ) );
		w.spin  = SpinFromParam( at( spin, t ) );
		w.churn = ChurnFromParam( at( churn, t ) );
		w.grain = GrainFromParam( at( grain, t ) );
		w.boil  = BoilFromParam( at( boil, t ) );

		w.density    = DensityFromParam( at( density, t ) );
		w.refraction = RefractionFromParam( at( refraction, t ) );
		w.dispersion = DispersionFromParam( at( dispersion, t ) );
		w.meniscus   = MeniscusFromParam( at( meniscus, t ) );
		w.caustic    = CausticFromParam( at( caustic, t ) );
		w.rim        = RimFromParam( at( rim, t ) );

		int paletteValue = 0;
		palette->getValueAtTime( t, paletteValue );
		w.palette = static_cast< Palette >( PaletteFromParam( static_cast< float >( paletteValue ) ) );

		w.hue        = HueFromParam( at( hue, t ) );
		w.hueSpread  = HueSpreadFromParam( at( hueSpread, t ) );
		w.saturation = SaturationFromParam( at( saturation, t ) );

		w.lamp        = LampFromParam( at( lamp, t ) );
		w.hotspot     = HotspotFromParam( at( hotspot, t ) );
		w.temperature = TemperatureFromParam( at( temperature, t ) );
		w.gate        = GateFromParam( at( gate, t ) );
		w.gateSoft    = GateSoftFromParam( at( gateSoft, t ) );

		//The aspect the CELLS are round in. Taken from the render window and
		//the pixel aspect together, because an anamorphic clip has square
		//pixels in neither sense and a wheel of ellipses is the one defect an
		//operator will blame on the plugin rather than on the format.
		const OfxRectI bounds = dst.getBounds();
		const double par      = dst.getPixelAspectRatio() > 0.0 ? dst.getPixelAspectRatio() : 1.0;
		const int outW        = bounds.x2 - bounds.x1;
		const int outH        = bounds.y2 - bounds.y1;
		w.aspect = outH > 0 ? static_cast< float >( outW * par / outH ) : 1.0f;

		//OFX time is FRAMES. Seconds come from the clip's frame rate, and a
		//host that reports zero -- some do, for a generator with nothing
		//connected -- would otherwise divide by it.
		double fps = dstClip->getFrameRate();
		if( !( fps > 0.0 ) && srcClip != nullptr )
			fps = srcClip->getFrameRate();
		if( !( fps > 0.0 ) )
			fps = 25.0;

		w.time = static_cast< float >( t / fps );

		//`time * boil` rather than an integrated phase. See the note at the
		//top of this file: there is no previous frame to have integrated
		//from, and a deterministic frame matters more in a host that renders
		//them out of order.
		w.boilPhase = w.time * w.boil;

		setup.cells.clear();
		const int count = std::min( std::max( w.cells, 0 ), kMaxCells );
		setup.cells.reserve( static_cast< size_t >( count ) );
		for( int i = 0; i < count; ++i )
			setup.cells.push_back( CellAt( i, w ) );

		setup.hasClip = overInput && src != nullptr;

		if( overInput )
		{
			int modeValue = 0;
			mode->getValueAtTime( t, modeValue );
			setup.mode = static_cast< LampMode >( ModeFromParam( static_cast< float >( modeValue ) ) );
			setup.mix  = static_cast< float >( at( mix, t ) );
		}

		setup.premultiplied =
			setup.hasClip && srcClip != nullptr
				? srcClip->getPreMultiplication() == OFX::eImagePreMultiplied
				: dstClip->getPreMultiplication() == OFX::eImagePreMultiplied;

		(void)args;
	}

	template< class Processor >
	void run( const OFX::RenderArguments& args, OFX::Image* dst, OFX::Image* src, const OilSetup& setup )
	{
		Processor processor( *this );
		processor.setDstImg( dst );
		processor.setSetup( src, &setup );
		processor.setRenderWindow( args.renderWindow );
		processor.process();
	}

	static float at( OFX::DoubleParam* p, double t )
	{
		double v = 0.0;
		p->getValueAtTime( t, v );
		return static_cast< float >( v );
	}

	//-----------------------------------------------------------------------
	// The preset binding. One list, in presets::Param order, so the table and
	// this file cannot drift apart without the static_assert firing.
	//-----------------------------------------------------------------------
	static const char* coveredName( int index )
	{
		static const char* const names[ presets::kParamCount ] = {
			kParamCells, kParamSize, kParamVariation, kParamMerge, kParamSpread, kParamScatter,
			kParamSpeed, kParamDrift, kParamSpin, kParamChurn, kParamGrain, kParamBoil,
			kParamDensity, kParamRefraction, kParamDispersion, kParamMeniscus, kParamCaustic, kParamRim,
			kParamPalette, kParamHue, kParamHueSpread, kParamSaturation,
			kParamLamp, kParamHotspot, kParamTemp, kParamGate, kParamGateSoft
		};
		return names[ index ];
	}

	OFX::DoubleParam* coveredDouble( int index ) const
	{
		switch( index )
		{
		case presets::kCells: return cells;
		case presets::kSize: return size;
		case presets::kVariation: return variation;
		case presets::kMerge: return merge;
		case presets::kSpread: return spread;
		case presets::kScatter: return scatter;
		case presets::kSpeed: return speed;
		case presets::kDrift: return drift;
		case presets::kSpin: return spin;
		case presets::kChurn: return churn;
		case presets::kGrain: return grain;
		case presets::kBoil: return boil;
		case presets::kDensity: return density;
		case presets::kRefraction: return refraction;
		case presets::kDispersion: return dispersion;
		case presets::kMeniscus: return meniscus;
		case presets::kCaustic: return caustic;
		case presets::kRim: return rim;
		case presets::kHue: return hue;
		case presets::kHueSpread: return hueSpread;
		case presets::kSaturation: return saturation;
		case presets::kLamp: return lamp;
		case presets::kHotspot: return hotspot;
		case presets::kTemperature: return temperature;
		case presets::kGate: return gate;
		case presets::kGateSoft: return gateSoft;
		default: return nullptr;//kPalette, which is a choice
		}
	}

	void setCovered( int index, float value )
	{
		if( index == presets::kPalette )
		{
			int current = 0;
			palette->getValue( current );
			const int wanted = static_cast< int >( std::lround( value ) );
			if( current != wanted )
				palette->setValue( wanted );
			return;
		}

		OFX::DoubleParam* p = coveredDouble( index );
		if( p == nullptr )
			return;

		double current = 0.0;
		p->getValue( current );
		if( std::fabs( current - static_cast< double >( value ) ) > 1e-6 )
			p->setValue( value );
	}

	bool coveredDiffers( int index, float value ) const
	{
		if( index == presets::kPalette )
		{
			int current = 0;
			palette->getValue( current );
			return current != static_cast< int >( std::lround( value ) );
		}

		OFX::DoubleParam* p = coveredDouble( index );
		if( p == nullptr )
			return false;

		double current = 0.0;
		p->getValue( current );
		return std::fabs( current - static_cast< double >( value ) ) > 1e-4;
	}

	const bool overInput;

	OFX::Clip* dstClip = nullptr;
	OFX::Clip* srcClip = nullptr;

	OFX::DoubleParam* cells       = nullptr;
	OFX::DoubleParam* size        = nullptr;
	OFX::DoubleParam* variation   = nullptr;
	OFX::DoubleParam* merge       = nullptr;
	OFX::DoubleParam* spread      = nullptr;
	OFX::DoubleParam* scatter     = nullptr;
	OFX::DoubleParam* seed        = nullptr;
	OFX::DoubleParam* speed       = nullptr;
	OFX::DoubleParam* drift       = nullptr;
	OFX::DoubleParam* spin        = nullptr;
	OFX::DoubleParam* churn       = nullptr;
	OFX::DoubleParam* grain       = nullptr;
	OFX::DoubleParam* boil        = nullptr;
	OFX::DoubleParam* density     = nullptr;
	OFX::DoubleParam* refraction  = nullptr;
	OFX::DoubleParam* dispersion  = nullptr;
	OFX::DoubleParam* meniscus    = nullptr;
	OFX::DoubleParam* caustic     = nullptr;
	OFX::DoubleParam* rim         = nullptr;
	OFX::ChoiceParam* palette     = nullptr;
	OFX::DoubleParam* hue         = nullptr;
	OFX::DoubleParam* hueSpread   = nullptr;
	OFX::DoubleParam* saturation  = nullptr;
	OFX::DoubleParam* lamp        = nullptr;
	OFX::DoubleParam* hotspot     = nullptr;
	OFX::DoubleParam* temperature = nullptr;
	OFX::DoubleParam* gate        = nullptr;
	OFX::DoubleParam* gateSoft    = nullptr;
	OFX::ChoiceParam* mode        = nullptr;
	OFX::DoubleParam* mix         = nullptr;
	OFX::ChoiceParam* preset      = nullptr;

	bool applyingPreset = false;
};

//---------------------------------------------------------------------------
OFX::DoubleParamDescriptor* defineSlider( OFX::ImageEffectDescriptor& desc,
                                          OFX::PageParamDescriptor* page,
                                          const char* name, const char* label,
                                          const char* hint, double defaultValue )
{
	OFX::DoubleParamDescriptor* p = desc.defineDoubleParam( name );
	p->setLabels( label, label, label );
	p->setHint( hint );
	p->setRange( 0.0, 1.0 );
	p->setDisplayRange( 0.0, 1.0 );
	p->setDefault( defaultValue );
	p->setIncrement( 0.001 );
	p->setDoubleType( OFX::eDoubleTypePlain );
	page->addChild( *p );
	return p;
}

void describeCommon( OFX::ImageEffectDescriptor& desc, const char* name )
{
	desc.setLabels( name, name, name );
	desc.setPluginGrouping( kPluginGrouping );
	desc.setPluginDescription( kPluginDescription );

	desc.addSupportedBitDepth( OFX::eBitDepthUByte );
	desc.addSupportedBitDepth( OFX::eBitDepthUShort );
	desc.addSupportedBitDepth( OFX::eBitDepthFloat );

	// Tiles declined. The cells are placed in FRAME space, so a tile cannot
	// know where they fall without the whole frame's geometry -- and the
	// refraction reads the clip at a displaced coordinate, which is a
	// dependency on pixels outside the tile that no region-of-interest
	// declaration here would bound honestly.
	desc.setSupportsTiles( false );

	// No temporal clip access: every frame is a pure function of its own
	// time. That is the whole design -- see the note at the top of this file.
	desc.setTemporalClipAccess( false );
	desc.setRenderThreadSafety( OFX::eRenderFullySafe );
	desc.setSupportsMultiResolution( true );
}

void describeParams( OFX::ImageEffectDescriptor& desc, bool lampVariant )
{
	OFX::PageParamDescriptor* page = desc.definePageParam( "Controls" );

	// Factory presets, from the same table the FFGL build reads (Presets.h).
	// Custom is not a preset: it means the sliders are the truth.
	OFX::ChoiceParamDescriptor* presetParam = desc.defineChoiceParam( kParamPreset );
	presetParam->setLabels( "Preset", "Preset", "Preset" );
	presetParam->setHint( "Named wheels. Picking one sets the covered controls; editing any "
	                      "of them afterwards falls back to Custom." );
	presetParam->appendOption( "Custom" );
	for( int i = 0; i < presets::kCount; ++i )
		presetParam->appendOption( presets::kPresets[ i ].name );
	presetParam->setDefault( 0 );
	presetParam->setIsPersistant( true );
	presetParam->setEvaluateOnChange( false );//the copied values re-render; the label does not
	presetParam->setAnimates( false );
	page->addChild( *presetParam );

	OFX::GroupParamDescriptor* wheelGroup = desc.defineGroupParam( "Wheel" );
	wheelGroup->setLabels( "Wheel", "Wheel", "Wheel" );

	defineSlider( desc, page, kParamCells, "Cells", "How many cells of oil, 1 to 48.", 0.86 )
		->setParent( *wheelGroup );
	defineSlider( desc, page, kParamSize, "Size",
	              "Average cell radius, as a fraction of half the short edge.", 0.62 )
		->setParent( *wheelGroup );
	defineSlider( desc, page, kParamVariation, "Variation", "Hashed spread of cell sizes.", 0.40 )
		->setParent( *wheelGroup );
	defineSlider( desc, page, kParamMerge, "Merge",
	              "The fillet where two cells meet. At 0 they stay separate beads.", 0.78 )
		->setParent( *wheelGroup );
	defineSlider( desc, page, kParamSpread, "Spread",
	              "How wide the cells are packed. Past 1 the outer ones leave the frame.", 0.85 )
		->setParent( *wheelGroup );
	defineSlider( desc, page, kParamScatter, "Scatter",
	              "Blends the even spiral packing towards a hashed swarm.", 0.30 )
		->setParent( *wheelGroup );
	defineSlider( desc, page, kParamSeed, "Seed", "A different wheel.", 0.0 )
		->setParent( *wheelGroup );

	OFX::GroupParamDescriptor* motionGroup = desc.defineGroupParam( "Motion" );
	motionGroup->setLabels( "Motion", "Motion", "Motion" );

	defineSlider( desc, page, kParamSpeed, "Speed", "Cycles per second; 0 stops the wheel.", 0.14 )
		->setParent( *motionGroup );
	defineSlider( desc, page, kParamDrift, "Drift", "How far each cell travels on its orbit.", 0.60 )
		->setParent( *motionGroup );
	defineSlider( desc, page, kParamSpin, "Spin",
	              "The whole wheel turning in its holder. 0.5 is still.", 0.50 )
		->setParent( *motionGroup );
	defineSlider( desc, page, kParamChurn, "Churn",
	              "How far the noise field deforms the oil, as a fraction of its wavelength.", 0.90 )
		->setParent( *motionGroup );
	defineSlider( desc, page, kParamGrain, "Grain", "How fine that noise is.", 0.70 )
		->setParent( *motionGroup );
	defineSlider( desc, page, kParamBoil, "Boil", "How fast it moves; 0 freezes it.", 0.20 )
		->setParent( *motionGroup );

	OFX::GroupParamDescriptor* opticsGroup = desc.defineGroupParam( "Optics" );
	opticsGroup->setLabels( "Optics", "Optics", "Optics" );

	defineSlider( desc, page, kParamDensity, "Density",
	              "How much light a cell's dye absorbs. Overlapping cells multiply.", 0.85 )
		->setParent( *opticsGroup );
	defineSlider( desc, page, kParamRefraction, "Refraction",
	              "How far the meniscus displaces what is behind it.", 0.62 )
		->setParent( *opticsGroup );
	defineSlider( desc, page, kParamDispersion, "Dispersion",
	              "Red and blue bent by different amounts: the fringe on a thick edge.", 0.30 )
		->setParent( *opticsGroup );
	defineSlider( desc, page, kParamMeniscus, "Meniscus",
	              "The dark line at a cell's edge, where light is thrown sideways.", 0.50 )
		->setParent( *opticsGroup );
	defineSlider( desc, page, kParamCaustic, "Caustic",
	              "The bright line just inside it, where light is concentrated.", 0.40 )
		->setParent( *opticsGroup );
	defineSlider( desc, page, kParamRim, "Rim", "How wide the rim treatment is.", 0.38 )
		->setParent( *opticsGroup );

	OFX::GroupParamDescriptor* colourGroup = desc.defineGroupParam( "Colour" );
	colourGroup->setLabels( "Colour", "Colour", "Colour" );

	OFX::ChoiceParamDescriptor* paletteParam = desc.defineChoiceParam( kParamPalette );
	paletteParam->setLabels( "Palette", "Palette", "Palette" );
	paletteParam->setHint( "The dye set. Aniline is the food-colouring pack; Ink is the "
	                       "subtractive primaries; Sodium is the hot end only." );
	for( int i = 0; i < int( Palette::Count ); ++i )
		paletteParam->appendOption( PaletteName( Palette( i ) ) );
	paletteParam->setDefault( 0 );
	paletteParam->setParent( *colourGroup );
	page->addChild( *paletteParam );

	defineSlider( desc, page, kParamHue, "Hue", "Rotates the whole palette.", 0.0 )
		->setParent( *colourGroup );
	defineSlider( desc, page, kParamHueSpread, "Hue Spread",
	              "How far apart the palette's dyes sit. At 0 the wheel is one colour.", 0.80 )
		->setParent( *colourGroup );
	defineSlider( desc, page, kParamSaturation, "Saturation",
	              "How strong the dyes are. At 0 the wheel is clear water.", 0.85 )
		->setParent( *colourGroup );

	OFX::GroupParamDescriptor* lampGroup = desc.defineGroupParam( "Lamp" );
	lampGroup->setLabels( "Lamp", "Lamp", "Lamp" );

	defineSlider( desc, page, kParamLamp, "Lamp", "Lamp brightness; 0.5 is unity.", 0.62 )
		->setParent( *lampGroup );
	defineSlider( desc, page, kParamHotspot, "Hotspot",
	              "The condenser's hot spot: how much brighter the middle is.", 0.20 )
		->setParent( *lampGroup );
	defineSlider( desc, page, kParamTemp, "Temperature",
	              "Cold discharge below 0.5, warm tungsten above.", 0.58 )
		->setParent( *lampGroup );
	defineSlider( desc, page, kParamGate, "Gate",
	              "The round gate's radius. At 1 it is off and the frame is full.", 1.0 )
		->setParent( *lampGroup );
	defineSlider( desc, page, kParamGateSoft, "Gate Soft", "How soft the gate's edge is.", 0.20 )
		->setParent( *lampGroup );

	if( lampVariant )
	{
		OFX::GroupParamDescriptor* output = desc.defineGroupParam( "Output" );
		output->setLabels( "Output", "Output", "Output" );

		OFX::ChoiceParamDescriptor* modeParam = desc.defineChoiceParam( kParamMode );
		modeParam->setLabels( "Mode", "Mode", "Mode" );
		modeParam->setHint( "Project puts the clip where the lamp was and looks through the oil "
		                    "at it; Over lights the oil itself and lays it on the clip; "
		                    "Colourise takes only the clip's brightness and lets the dye supply "
		                    "the colour." );
		for( int i = 0; i < int( LampMode::Count ); ++i )
			modeParam->appendOption( LampModeName( LampMode( i ) ) );
		modeParam->setDefault( 0 );
		modeParam->setParent( *output );
		page->addChild( *modeParam );

		defineSlider( desc, page, kParamMix, "Mix", "Dry/wet with the untouched clip.", 1.0 )
			->setParent( *output );
	}

	// The Stoatworks About block: a read-only credit line and one push button
	// per link, in a group that starts folded. Last, so it sits under the
	// effect's own controls.
	stoatworks::about::ofx::describe( desc, page );
}

} // namespace

//---------------------------------------------------------------------------
// "Flenser": the generator.
//---------------------------------------------------------------------------
mDeclarePluginFactory( FlenserSourceFactory, {}, {} );

void FlenserSourceFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	describeCommon( desc, "Flenser" );
	desc.addSupportedContext( OFX::eContextGenerator );
	desc.addSupportedContext( OFX::eContextGeneral );
}

void FlenserSourceFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	describeParams( desc, false );
}

OFX::ImageEffect* FlenserSourceFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new FlenserOFXPlugin( handle, false );
}

//---------------------------------------------------------------------------
// "Flenser Lamp": the effect.
//---------------------------------------------------------------------------
mDeclarePluginFactory( FlenserLampFactory, {}, {} );

void FlenserLampFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	describeCommon( desc, "Flenser Lamp" );
	desc.addSupportedContext( OFX::eContextFilter );
	desc.addSupportedContext( OFX::eContextGeneral );
}

void FlenserLampFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	OFX::ClipDescriptor* srcClip = desc.defineClip( kOfxImageEffectSimpleSourceClipName );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGB );
	srcClip->setSupportsTiles( false );

	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	describeParams( desc, true );
}

OFX::ImageEffect* FlenserLampFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new FlenserOFXPlugin( handle, true );
}

void OFX::Plugin::getPluginIDs( OFX::PluginFactoryArray& ids )
{
	// Deliberately leaked: a by-value static would register an exit-time
	// destructor inside this module, and a host that dlclose()s the bundle
	// before process exit then jumps through a dangling pointer.
	static FlenserSourceFactory* sourceFactory =
		new FlenserSourceFactory( kSourceIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	static FlenserLampFactory* lampFactory =
		new FlenserLampFactory( kLampIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	ids.push_back( sourceFactory );
	ids.push_back( lampFactory );
}
