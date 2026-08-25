#include "Oil.h"

#include "Hash.h"

#include <algorithm>
#include <cmath>

namespace flenser
{
namespace
{
constexpr float kTau = 6.28318530717958647692f;

/// The golden angle, in radians. `pi * (3 - sqrt(5))`.
///
/// Placing cell `i` at angle `i * kGoldenAngle` and radius `sqrt(i/n)` is the
/// phyllotactic packing -- the arrangement a sunflower head uses, and the one
/// a dish full of equal cells pressing on each other settles into. It has the
/// property that matters here: it stays even at EVERY count, so changing
/// Cells from nine to ten rearranges nothing, it adds one.
constexpr float kGoldenAngle = 2.39996322972865332f;

/// GLSL's `mix`, written the way GLSL specifies it: `x*(1-a) + y*a`.
///
/// Not `x + (y-x)*a`. The two are algebraically identical and are NOT the
/// same sequence of floating-point operations, and this function is compared
/// against a shader's `mix` to five decimal places by `fltest --field`.
///= mirrored
inline float mixf( float x, float y, float a )
{
	return x * ( 1.0f - a ) + y * a;
}

///= mirrored
inline float clampf( float x, float lo, float hi )
{
	return std::min( std::max( x, lo ), hi );
}

/// GLSL's `smoothstep`, written the way GLSL specifies it.
///= mirrored
inline float smoothstepf( float e0, float e1, float x )
{
	const float t = clampf( ( x - e0 ) / ( e1 - e0 ), 0.0f, 1.0f );
	return t * t * ( 3.0f - 2.0f * t );
}

/// The polynomial smooth minimum, and the blend weight it used.
///
/// `h` is the weight of `a`: 1 when a wins outright, 0 when b does. The
/// caller needs it because the normal has to be blended by the same weight
/// the distance was, and recomputing it would be a second divide in the
/// innermost loop of both builds.
///
/// The `k <= 0` branch is not defensive tidying. Merge maps to exactly zero
/// at the bottom of its travel (Controls.cpp), and this expression divides by
/// k -- so the branch is the control's null, and it has to exist identically
/// in the shader or the two builds disagree at one end of one slider.
///= mirrored
inline float smin( float a, float b, float k, float& outH )
{
	if( k <= 0.0f )
	{
		outH = a < b ? 1.0f : 0.0f;
		return std::min( a, b );
	}

	outH = clampf( 0.5f + 0.5f * ( b - a ) / k, 0.0f, 1.0f );
	return mixf( b, a, outH ) - k * outH * ( 1.0f - outH );
}

/// The palettes, as hue offsets in turns. Scaled by Hue Spread and rotated by
/// Hue, so what a palette fixes is the RELATIONSHIP between the dyes.
///
/// Aniline is the four bottles in a supermarket food-colouring pack, which is
/// what the DIY version of this is actually made with. Ink is the subtractive
/// primaries, which is what the effect is physically doing anyway and so the
/// set that overlaps most cleanly. Sodium is the hot end only.
const float kAniline[] = { 0.00f, 0.33f, 0.58f, 0.13f };
const float kInk[]     = { 0.50f, 0.83f, 0.16f };
const float kSodium[]  = { 0.02f, 0.06f, 0.11f };
const float kDuotone[] = { 0.00f, 0.50f };
const float kMono[]    = { 0.00f };

struct PaletteTable
{
	const float* offsets;
	int count;
};

PaletteTable tableFor( Palette palette )
{
	switch( palette )
	{
	case Palette::Aniline: return { kAniline, 4 };
	case Palette::Ink: return { kInk, 3 };
	case Palette::Sodium: return { kSodium, 3 };
	case Palette::Duotone: return { kDuotone, 2 };
	case Palette::Mono: return { kMono, 1 };
	default: return { kAniline, 4 };
	}
}
} // namespace

const char* PaletteName( Palette value )
{
	switch( value )
	{
	case Palette::Aniline: return "Aniline";
	case Palette::Ink: return "Ink";
	case Palette::Sodium: return "Sodium";
	case Palette::Spectrum: return "Spectrum";
	case Palette::Duotone: return "Duotone";
	case Palette::Mono: return "Mono";
	default: return "Aniline";
	}
}

const char* LampModeName( LampMode value )
{
	switch( value )
	{
	case LampMode::Project: return "Project";
	case LampMode::Over: return "Over";
	case LampMode::Colourise: return "Colourise";
	default: return "Project";
	}
}

//---------------------------------------------------------------------------
void HsvToRgb( float h, float s, float v, float& outR, float& outG, float& outB )
{
	h = h - std::floor( h );
	s = clampf( s, 0.0f, 1.0f );

	const float sector = h * 6.0f;
	const float i      = std::floor( sector );
	const float f      = sector - i;

	const float p = v * ( 1.0f - s );
	const float q = v * ( 1.0f - s * f );
	const float t = v * ( 1.0f - s * ( 1.0f - f ) );

	switch( static_cast< int >( i ) % 6 )
	{
	case 0: outR = v; outG = t; outB = p; break;
	case 1: outR = q; outG = v; outB = p; break;
	case 2: outR = p; outG = v; outB = t; break;
	case 3: outR = p; outG = q; outB = v; break;
	case 4: outR = t; outG = p; outB = v; break;
	default: outR = v; outG = p; outB = q; break;
	}
}

//---------------------------------------------------------------------------
Cell CellAt( int index, const Wheel& wheel )
{
	Cell cell;

	const int count       = std::max( wheel.cells, 1 );
	const uint32_t seed   = static_cast< uint32_t >( std::max( wheel.seed, 0 ) );
	const uint32_t i      = static_cast< uint32_t >( std::max( index, 0 ) );
	const uint32_t hPos   = Hash2( i, seed );
	const uint32_t hSize  = Hash3( i, seed, 0x51u );
	const uint32_t hOrbit = Hash3( i, seed, 0xa7u );
	const uint32_t hDye   = Hash3( i, seed, 0x1du );

	//-- where it sits ---------------------------------------------------
	//
	// Two arrangements of the same disc, blended by Scatter. The spiral is
	// equal-AREA -- radius goes as the square root of the index fraction --
	// because a spiral that spaced its radii evenly would crowd the middle
	// and thin out at the rim, which is the opposite of what a squeezed dish
	// does.
	const float k       = ( static_cast< float >( index ) + 0.5f ) / static_cast< float >( count );
	const float spiralR = std::sqrt( clampf( k, 0.0f, 1.0f ) ) * wheel.spread;
	const float spiralA = static_cast< float >( index ) * kGoldenAngle;

	const float swarmR = std::sqrt( Unit( hPos ) ) * wheel.spread;
	const float swarmA = Unit( hOrbit ) * kTau;

	float px = mixf( spiralR * std::cos( spiralA ), swarmR * std::cos( swarmA ), wheel.scatter );
	float py = mixf( spiralR * std::sin( spiralA ), swarmR * std::sin( swarmA ), wheel.scatter );

	//-- how it moves ----------------------------------------------------
	//
	// A closed orbit per cell, on two INCOMMENSURATE frequencies. Integer
	// ratios would give every cell a short repeating figure and the whole
	// wheel a period; a real dish never comes back to where it was, and a
	// projection that loops every four seconds is the one thing an audience
	// does notice.
	const float t     = wheel.time * wheel.speed;
	const float f1    = 0.55f + 0.90f * Unit( hOrbit );
	const float f2    = 0.50f + 1.10f * Unit( hDye );
	const float phase = Unit( hPos );

	px += wheel.drift * std::cos( kTau * ( f1 * t + phase ) );
	py += wheel.drift * std::sin( kTau * ( f2 * t + phase * 1.37f + 0.21f ) );

	//-- the wheel turning in its holder ---------------------------------
	const float a  = kTau * wheel.spin * wheel.time;
	const float ca = std::cos( a );
	const float sa = std::sin( a );

	cell.x = px * ca - py * sa;
	cell.y = px * sa + py * ca;

	//-- how big it is ---------------------------------------------------
	//
	// The breathe is driven by `t`, which already carries Speed, so a stopped
	// wheel is stopped in every respect. A pulse tied to the wall clock
	// instead would leave the cells quietly inflating with Speed at zero.
	float radius = wheel.size * ( 1.0f + 0.9f * wheel.variation * Signed( hSize ) );
	radius *= 1.0f + 0.12f * std::sin( kTau * ( 0.31f * t + Unit( hSize ) ) );
	cell.radius = std::max( radius, 0.002f );

	//-- what colour it dyes ---------------------------------------------
	//
	// Every cell gets a hashed dye STRENGTH as well as a hue, and it is not
	// decoration. Nobody charges a wheel with six identical concentrations:
	// the dye goes in drop by drop, some cells take it and some are mostly
	// carrier, and a wheel where every cell absorbs exactly as hard as every
	// other reads as printed shapes rather than as liquid. It costs one hash
	// and one lerp, per cell, on the CPU.
	//
	// The range is narrow on purpose. Wide enough and the wheel stops reading
	// as one charge of dye and starts reading as several plugins; this is the
	// difference between "some of these took the colour better" and "half of
	// them are a different effect".
	const uint32_t hStrength = Hash3( i, seed, 0xc3u );
	const float strength     = 0.72f + 0.28f * Unit( hStrength );

	float offset;
	if( wheel.palette == Palette::Spectrum )
	{
		offset = Unit( hDye );
	}
	else
	{
		const PaletteTable table = tableFor( wheel.palette );
		const int pick           = static_cast< int >( Hash32( hDye ) % static_cast< uint32_t >( table.count ) );
		offset                   = table.offsets[ pick ];
	}

	HsvToRgb( wheel.hue + wheel.hueSpread * offset, wheel.saturation, 1.0f,
	          cell.dye[ 0 ], cell.dye[ 1 ], cell.dye[ 2 ] );

	//Towards clear, by this cell's strength. A transmittance of 1 is clear
	//glass, so this is the correct direction to pull a weak dye in -- pulling
	//it towards black instead would make a pale cell a dark one.
	for( int channel = 0; channel < 3; ++channel )
		cell.dye[ channel ] = mixf( 1.0f, cell.dye[ channel ], strength );

	return cell;
}

//===========================================================================
// The mirrored per-pixel stage. Every function below has a transcription in
// `kOilLibrary` in Shaders.cpp. Change one, change both, and run
// `tools/fltest --field`.
//===========================================================================

///= mirrored
float Noise2( float x, float y )
{
	const float fx = std::floor( x );
	const float fy = std::floor( y );

	//int32 -> uint32 keeps the bit pattern in C++ and in GLSL alike, so a
	//lattice cell at a negative coordinate hashes to the same value on both
	//sides. Anything that "fixed" the negatives by adding a large offset
	//would have to add exactly the same one in the shader.
	const uint32_t ix = static_cast< uint32_t >( static_cast< int32_t >( fx ) );
	const uint32_t iy = static_cast< uint32_t >( static_cast< int32_t >( fy ) );

	const float tx = x - fx;
	const float ty = y - fy;

	//Quintic fade. Its first AND second derivatives vanish at the ends, so
	//two octaves laid over each other have no visible lattice; the cubic
	//`t*t*(3-2t)` leaves a grid of creases that reads as a mesh over the oil.
	const float ux = tx * tx * tx * ( tx * ( tx * 6.0f - 15.0f ) + 10.0f );
	const float uy = ty * ty * ty * ( ty * ( ty * 6.0f - 15.0f ) + 10.0f );

	const float n00 = Signed( Hash2( ix, iy ) );
	const float n10 = Signed( Hash2( ix + 1u, iy ) );
	const float n01 = Signed( Hash2( ix, iy + 1u ) );
	const float n11 = Signed( Hash2( ix + 1u, iy + 1u ) );

	return mixf( mixf( n00, n10, ux ), mixf( n01, n11, ux ), uy );
}

///= mirrored
void WarpPoint( float x, float y, float churn, float grain, float boilPhase,
                float& outX, float& outY )
{
	if( churn <= 0.0f )
	{
		outX = x;
		outY = y;
		return;
	}

	//Churn is a fraction of the noise's own WAVELENGTH, not an absolute
	//displacement -- so the amplitude is divided by the grain here rather
	//than being handed over ready-made.
	//
	//That division is the difference between a control that deforms the oil
	//and one that destroys it. A fixed displacement large against the noise's
	//feature size FOLDS the plane: the field stops being a distance field,
	//the rim band's width varies wildly with position, and a cell comes out
	//as a nest of contour lines rather than as a shape. Tying the two
	//together means Churn does the same kind of thing at every Grain, and
	//cannot reach the folding regime at all.
	const float amp = churn / std::max( grain, 1.0e-3f );

	//Two octaves going opposite ways. The second is at 2.03x rather than 2x
	//so the two lattices never line up; at exactly two they share every other
	//grid line and the sum has a visible square structure.
	const float sx = boilPhase * 0.31f;
	const float sy = boilPhase * 0.21f;
	const float tx = boilPhase * 0.17f;
	const float ty = boilPhase * 0.44f;

	const float ax = Noise2( x * grain + sx, y * grain + sy );
	const float ay = Noise2( x * grain + sx + 37.0f, y * grain + sy - 19.0f );

	const float bx = Noise2( x * grain * 2.03f - tx, y * grain * 2.03f - ty );
	const float by = Noise2( x * grain * 2.03f - tx + 11.0f, y * grain * 2.03f - ty + 53.0f );

	outX = x + amp * ( ax + 0.5f * bx );
	outY = y + amp * ( ay + 0.5f * by );
}

///= mirrored
Sample FieldAt( float x, float y, const Cell* cells, int count,
                float merge, float density, float rim )
{
	Sample out;

	if( count <= 0 )
	{
		//An empty wheel is clear glass: no surface anywhere, full
		//transmittance. 1e6 rather than a float infinity because the shader's
		//mirror of this is a literal and GLSL has no portable infinity.
		out.d  = 1.0e6f;
		out.dn = 1.0e6f;
		return out;
	}

	const float covWidth = std::max( rim, 1.0e-5f );

	for( int i = 0; i < count; ++i )
	{
		const float dx  = x - cells[ i ].x;
		const float dy  = y - cells[ i ].y;
		const float len = std::sqrt( dx * dx + dy * dy );
		const float di  = len - cells[ i ].radius;

		//At the exact centre of a cell there is no direction to point in.
		//Zero is the only answer that does not depend on the rounding of a
		//division by something near zero -- and it is one pixel, once, at the
		//middle of a cell where the rim treatment is zero anyway.
		const float inv = len > 1.0e-6f ? 1.0f / len : 0.0f;
		const float gix = dx * inv;
		const float giy = dy * inv;

		//The nearest single boundary, by MAGNITUDE. Deep inside one cell and
		//just under the edge of another, this picks the edge -- which is the
		//point of it.
		if( i == 0 || std::fabs( di ) < std::fabs( out.dn ) )
			out.dn = di;

		if( i == 0 )
		{
			out.d  = di;
			out.gx = gix;
			out.gy = giy;
		}
		else
		{
			float h = 0.0f;
			out.d   = smin( out.d, di, merge, h );

			//`h` is the weight of the ACCUMULATED side, so the new cell's
			//normal is the `x` argument of mix and the accumulated one is
			//`y`. Getting this round the wrong way is invisible in a still
			//frame with separated cells and wrong everywhere they touch.
			out.gx = mixf( gix, out.gx, h );
			out.gy = mixf( giy, out.gy, h );
		}

		//The dye stack. MULTIPLIED, which is the whole reason this looks like
		//a dyed wheel rather than like coloured blobs: two cells overlapping
		//subtract twice, so cyan over magenta is blue and not white.
		const float cov = smoothstepf( 0.0f, covWidth, -di ) * density;
		out.t[ 0 ] *= mixf( 1.0f, cells[ i ].dye[ 0 ], cov );
		out.t[ 1 ] *= mixf( 1.0f, cells[ i ].dye[ 1 ], cov );
		out.t[ 2 ] *= mixf( 1.0f, cells[ i ].dye[ 2 ], cov );
	}

	return out;
}

///= mirrored
void SynthLamp( float x, float y, float lamp, float hotspot, float temperature,
                float& outR, float& outG, float& outB )
{
	const float r2 = x * x + y * y;

	//A Fresnel condenser's falloff, near enough: a Gaussian in the radius.
	//The exponent is picked so that at the corner of a 16:9 frame the edge is
	//about a fifth of the centre with Hotspot at 1, which is what an overhead
	//projector actually measures like.
	const float hot = mixf( 1.0f, std::exp( -1.35f * r2 ), hotspot );

	//Tungsten one way, metal halide the other. Not a black-body calculation:
	//these are the two ends the format actually has, and interpolating
	//between two measured white points is both cheaper and closer than a
	//Planckian locus evaluated per pixel.
	const float warmR = 1.00f, warmG = 0.82f, warmB = 0.62f;
	const float coolR = 0.78f, coolG = 0.88f, coolB = 1.00f;

	const float t = clampf( temperature, -1.0f, 1.0f );
	const float w = std::max( t, 0.0f );
	const float c = std::max( -t, 0.0f );

	const float tintR = mixf( mixf( 1.0f, warmR, w ), coolR, c );
	const float tintG = mixf( mixf( 1.0f, warmG, w ), coolG, c );
	const float tintB = mixf( mixf( 1.0f, warmB, w ), coolB, c );

	outR = lamp * hot * tintR;
	outG = lamp * hot * tintG;
	outB = lamp * hot * tintB;
}

///= mirrored
float GateAt( float x, float y, float gate, float gateSoft )
{
	const float r = std::sqrt( x * x + y * y );

	//The floor on the softness is load-bearing rather than cosmetic. GLSL
	//leaves smoothstep UNDEFINED when its two edges are equal, and Gate Soft
	//maps to exactly 0 at the bottom of its travel -- so without this the
	//hard-edged gate, which is the most useful setting the control has, is
	//whatever the driver felt like.
	const float soft  = std::max( gateSoft, 1.0e-3f );
	const float inner = gate * ( 1.0f - soft );

	return 1.0f - smoothstepf( inner, gate, r );
}

///= mirrored
void RimProfiles( float d, float rim, float& outCaustic, float& outMeniscus )
{
	const float w = std::max( rim, 1.0e-5f );

	//The caustic peaks INSIDE the surface, at about two thirds of the rim
	//width in, because that is where light bent by the meniscus lands. The
	//meniscus line peaks ON it, where the boundary is steepest and most of
	//the light is being thrown sideways instead of forward.
	const float ec = ( d + 0.65f * w ) / ( 0.55f * w );
	const float em = d / ( 0.50f * w );

	outCaustic  = std::exp( -ec * ec );
	outMeniscus = std::exp( -em * em );
}

///= mirrored
void BendAt( const Sample& s, float refraction, float rim, float& outX, float& outY )
{
	if( refraction <= 0.0f )
	{
		outX = 0.0f;
		outY = 0.0f;
		return;
	}

	const float w = std::max( rim, 1.0e-5f );
	const float e = s.dn / w;

	//A band on EVERY surface and nothing elsewhere. `dn` and not `d`, so that
	//a cell lying over another one refracts through its own edge as well as
	//through the pile's outline. The middle of a cell is
	//flat oil between two flat glasses and bends light by almost nothing; all
	//the optics are in the meniscus. Distorting the whole cell instead --
	//which is what a displacement proportional to depth would do -- gives the
	//lava-lamp look this plugin exists to avoid.
	const float lens = std::exp( -e * e );

	outX = refraction * lens * s.gx;
	outY = refraction * lens * s.gy;
}

} // namespace flenser
