#include "Shaders.h"

#include "Oil.h"

namespace flenser
{

const char* const kVertexShader = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;

	//Straight through, in 0..1 picture space. MaxUV is applied where the
	//incoming clip is actually sampled, because this plugin samples it at a
	//DISPLACED coordinate -- folding MaxUV in here would scale the
	//displacement too, and the refraction would get stronger the more padding
	//the host's texture happened to have.
	uv = vUV;
}
)";

//---------------------------------------------------------------------------
// The mirrored library. See Oil.cpp; every line marked there is marked here.
//---------------------------------------------------------------------------
const char* const kOilLibrary = R"(
const float kTau = 6.283185307179586;

//The wheel, as the CPU resolved it. xy is the centre in wheel space and z is
//the radius; the second array is the dye's transmittance.
uniform int CellCount;
uniform vec3 CellPos[ 48 ];
uniform vec3 CellDye[ 48 ];

struct Field
{
	float d; //the union's surface: filleted by Merge, and what the normal comes from
	float dn;//the NEAREST single boundary: not filleted, and what the rim comes from
	vec2 g;
	vec3 t;
};

//= mirrored
uint hash32( uint x )
{
	x ^= x >> 16u;
	x *= 0x7feb352du;
	x ^= x >> 15u;
	x *= 0x846ca68bu;
	x ^= x >> 16u;
	return x;
}

//= mirrored
uint hash2( uint a, uint b )
{
	return hash32( a ^ hash32( b + 0x9e3779b9u ) );
}

//= mirrored
float unitOf( uint h )
{
	//The top 24 bits: the widest slice that converts to a float32 without
	//rounding. Taking the low bits instead would round on every value, which
	//is exactly the sort of difference `--field` reports and nobody can
	//explain.
	return float( h >> 8u ) * ( 1.0 / 16777216.0 );
}

//= mirrored
float signedOf( uint h )
{
	return unitOf( h ) * 2.0 - 1.0;
}

//= mirrored
float smin( float a, float b, float k, out float outH )
{
	//Merge maps to EXACTLY zero at the bottom of its travel, and the
	//expression below divides by k. This branch is the control's null.
	if( k <= 0.0 )
	{
		outH = a < b ? 1.0 : 0.0;
		return min( a, b );
	}

	outH = clamp( 0.5 + 0.5 * ( b - a ) / k, 0.0, 1.0 );
	return mix( b, a, outH ) - k * outH * ( 1.0 - outH );
}

//= mirrored
float noise2( float x, float y )
{
	float fx = floor( x );
	float fy = floor( y );

	//int -> uint keeps the bit pattern here exactly as it does in C++, so a
	//lattice cell at a negative coordinate hashes to the same value on both
	//sides.
	uint ix = uint( int( fx ) );
	uint iy = uint( int( fy ) );

	float tx = x - fx;
	float ty = y - fy;

	//Quintic fade: first AND second derivatives vanish at the ends, so two
	//octaves laid over each other leave no visible lattice.
	float ux = tx * tx * tx * ( tx * ( tx * 6.0 - 15.0 ) + 10.0 );
	float uy = ty * ty * ty * ( ty * ( ty * 6.0 - 15.0 ) + 10.0 );

	float n00 = signedOf( hash2( ix, iy ) );
	float n10 = signedOf( hash2( ix + 1u, iy ) );
	float n01 = signedOf( hash2( ix, iy + 1u ) );
	float n11 = signedOf( hash2( ix + 1u, iy + 1u ) );

	return mix( mix( n00, n10, ux ), mix( n01, n11, ux ), uy );
}

//= mirrored
vec2 warpPoint( vec2 p, float churn, float grain, float boilPhase )
{
	if( churn <= 0.0 )
		return p;

	//Churn is a fraction of the noise's own WAVELENGTH: a fixed displacement
	//large against the feature size folds the plane, the field stops being a
	//distance field, and a cell comes out as a nest of contour lines rather
	//than as a shape. See Oil.cpp.
	float amp = churn / max( grain, 1.0e-3 );

	//Two octaves going opposite ways. The second is at 2.03x rather than 2x
	//so the two lattices never line up; at exactly two they share every other
	//grid line and the sum has a visible square structure.
	float sx = boilPhase * 0.31;
	float sy = boilPhase * 0.21;
	float tx = boilPhase * 0.17;
	float ty = boilPhase * 0.44;

	float ax = noise2( p.x * grain + sx, p.y * grain + sy );
	float ay = noise2( p.x * grain + sx + 37.0, p.y * grain + sy - 19.0 );

	float bx = noise2( p.x * grain * 2.03 - tx, p.y * grain * 2.03 - ty );
	float by = noise2( p.x * grain * 2.03 - tx + 11.0, p.y * grain * 2.03 - ty + 53.0 );

	return vec2( p.x + amp * ( ax + 0.5 * bx ),
	             p.y + amp * ( ay + 0.5 * by ) );
}

//= mirrored
Field fieldAt( vec2 p, float merge, float density, float rim )
{
	Field f;
	f.d  = 1.0e6;
	f.dn = 1.0e6;
	f.g  = vec2( 0.0 );
	f.t  = vec3( 1.0 );

	if( CellCount <= 0 )
		return f;

	float covWidth = max( rim, 1.0e-5 );

	//A constant bound with a break, because GLSL will not unroll a loop whose
	//count is a uniform and some drivers will not compile one at all.
	for( int i = 0; i < 48; ++i )
	{
		if( i >= CellCount )
			break;

		vec2 delta = p - CellPos[ i ].xy;
		float len  = sqrt( delta.x * delta.x + delta.y * delta.y );
		float di   = len - CellPos[ i ].z;

		//At the exact centre of a cell there is no direction to point in.
		float inv = len > 1.0e-6 ? 1.0 / len : 0.0;
		vec2 gi   = delta * inv;

		//The nearest single boundary, by MAGNITUDE. Deep inside one cell and
		//just under the edge of another, this picks the edge -- which is the
		//point of it, and is why the rim is drawn round every cell rather
		//than only round the outside of the pile.
		if( i == 0 || abs( di ) < abs( f.dn ) )
			f.dn = di;

		if( i == 0 )
		{
			f.d = di;
			f.g = gi;
		}
		else
		{
			float h = 0.0;
			f.d     = smin( f.d, di, merge, h );

			//`h` is the weight of the ACCUMULATED side, so the new cell's
			//normal is the first argument of mix and the accumulated one is
			//the second.
			f.g = mix( gi, f.g, h );
		}

		//The dye stack. MULTIPLIED, which is the whole reason this looks like
		//a dyed wheel rather than like coloured blobs.
		float cov = smoothstep( 0.0, covWidth, -di ) * density;
		f.t *= mix( vec3( 1.0 ), CellDye[ i ], cov );
	}

	return f;
}

//= mirrored
vec3 synthLamp( vec2 p, float lamp, float hotspot, float temperature )
{
	float r2 = p.x * p.x + p.y * p.y;

	float hot = mix( 1.0, exp( -1.35 * r2 ), hotspot );

	const vec3 warm = vec3( 1.00, 0.82, 0.62 );
	const vec3 cool = vec3( 0.78, 0.88, 1.00 );

	float t = clamp( temperature, -1.0, 1.0 );
	float w = max( t, 0.0 );
	float c = max( -t, 0.0 );

	vec3 tint = mix( mix( vec3( 1.0 ), warm, w ), cool, c );

	return lamp * hot * tint;
}

//= mirrored
float gateAt( vec2 p, float gate, float gateSoft )
{
	float r = sqrt( p.x * p.x + p.y * p.y );

	//GLSL leaves smoothstep UNDEFINED when its two edges are equal, and Gate
	//Soft maps to exactly 0 at the bottom of its travel -- so without this
	//floor the hard-edged gate is whatever the driver felt like.
	float soft  = max( gateSoft, 1.0e-3 );
	float inner = gate * ( 1.0 - soft );

	return 1.0 - smoothstep( inner, gate, r );
}

//= mirrored
void rimProfiles( float d, float rim, out float outCaustic, out float outMeniscus )
{
	float w = max( rim, 1.0e-5 );

	float ec = ( d + 0.65 * w ) / ( 0.55 * w );
	float em = d / ( 0.50 * w );

	outCaustic  = exp( -ec * ec );
	outMeniscus = exp( -em * em );
}

//= mirrored
vec2 bendAt( Field f, float refraction, float rim )
{
	if( refraction <= 0.0 )
		return vec2( 0.0 );

	float w = max( rim, 1.0e-5 );
	float e = f.dn / w;

	//A band on EVERY surface and nothing elsewhere. The middle of a cell is
	//flat oil between two flat glasses and bends light by almost nothing --
	//and `dn` rather than `d`, so a cell lying over another one refracts
	//through its own edge as well as through the pile's outline.
	float lens = exp( -e * e );

	return refraction * lens * f.g;
}
)";

//---------------------------------------------------------------------------
// Pass 1: oil. Everything expensive, once.
//---------------------------------------------------------------------------
static const char* const kOilPreamble = R"(#version 410 core

uniform sampler2D InputTexture;
uniform vec2 MaxUV;     //the part of the input texture that is really picture
uniform vec2 HalfTexel; //half an input texel, in picture space

uniform float Aspect;

uniform float Merge;
uniform float Density;
uniform float Rim;
uniform float Churn;
uniform float Grain;
uniform float BoilPhase;

uniform float Refraction;
uniform float Dispersion;
uniform float Meniscus;
uniform float Caustic;

uniform float Lamp;
uniform float Hotspot;
uniform float Temperature;
uniform float Gate;
uniform float GateSoft;

uniform int Mode;    //0 Project, 1 Over, 2 Colourise
uniform int HasClip; //1 when this build has an input clip connected

in vec2 uv;
out vec4 fragColor;
)";

static const char* const kOilMain = R"(
//Rec.709 luma. Used in two places -- how bright a caustic is, and what the
//clip contributes in Colourise -- and both want the same answer.
float luma( vec3 c )
{
	return dot( c, vec3( 0.2126, 0.7152, 0.0722 ) );
}

vec4 sampleClip( vec2 at )
{
	//Half a texel in from the edge, THEN scaled into the host's texture. The
	//clamp is not belt and braces: the refraction deliberately samples off
	//the side of the picture at the rim of a cell that overhangs the frame,
	//and GL_LINEAR at the boundary would take half its weight from the
	//texture's undrawn padding -- a black fringe following every cell edge
	//round the border of the frame.
	vec2 picture = clamp( at, HalfTexel, vec2( 1.0 ) - HalfTexel );
	return texture( InputTexture, picture * MaxUV );
}

void main()
{
	vec2 p = vec2( ( uv.x - 0.5 ) * 2.0 * Aspect, ( uv.y - 0.5 ) * 2.0 );

	//The churn moves the OIL. The gate does not move with it: the gate is a
	//hole in the projector's casting and the oil is on the glass above it, so
	//a churn strong enough to slosh the picture must not slosh the aperture
	//as well.
	vec2 pw = warpPoint( p, Churn, Grain, BoilPhase );

	Field f    = fieldAt( pw, Merge, Density, Rim );
	vec2 bend  = bendAt( f, Refraction, Rim );
	float gate = gateAt( p, Gate, GateSoft );

	//From the nearest single boundary, not from the union's surface. See the
	//note on Field.dn in Oil.h: drawn from `d` this is one meniscus round the
	//outside of the whole pile, and a dozen cells come out as one flat
	//coloured shape with the light doing nothing anywhere inside it.
	float cst, men;
	rimProfiles( f.dn, Rim, cst, men );

	//Wheel units into picture units. The short edge is 2 wheel units, so a
	//displacement of 0.02 is one per cent of the picture height whatever the
	//aspect ratio is -- which is why the cells stay round on a 21:9 canvas
	//and the refraction stays circular with them.
	vec2 bendUv = vec2( bend.x / ( 2.0 * Aspect ), bend.y * 0.5 );

	//Dispersion: red and blue are bent by different amounts, which is what
	//makes the coloured fringe on a thick edge of oil. Green is left alone so
	//that the picture does not appear to move as the control opens.
	float dr = 1.0 + 0.30 * Dispersion;
	float db = 1.0 - 0.30 * Dispersion;

	vec3 light;
	vec4 clip = vec4( 0.0 );

	if( HasClip == 1 )
		clip = sampleClip( uv );

	if( HasClip == 1 && Mode == 0 )
	{
		//Project: the clip IS the lamp, so it is what gets bent -- three
		//samples, one per channel, at three displacements.
		light = vec3( sampleClip( uv - bendUv * dr ).r,
		              sampleClip( uv - bendUv ).g,
		              sampleClip( uv - bendUv * db ).b );

		//The projector's own optics still apply to it: a hot spot, a colour
		//temperature and however hard the lamp is being driven.
		light *= synthLamp( p, Lamp, Hotspot, Temperature );
	}
	else if( HasClip == 1 && Mode == 2 )
	{
		//Colourise: the clip supplies brightness and nothing else. One sample
		//-- there is no colour in it left to disperse.
		float lit = luma( sampleClip( uv - bendUv ).rgb );
		light     = lit * synthLamp( p, Lamp, Hotspot, Temperature );
	}
	else
	{
		//The generator, and the Over mode: the plugin's own lamp, bent by the
		//same meniscus. Sampling an analytic lamp at a displaced point costs
		//nothing, so it gets the same three-channel dispersion the clip does.
		light = vec3( synthLamp( p - bend * dr, Lamp, Hotspot, Temperature ).r,
		              synthLamp( p - bend, Lamp, Hotspot, Temperature ).g,
		              synthLamp( p - bend * db, Lamp, Hotspot, Temperature ).b );
	}

	//---- the dye stack ------------------------------------------------
	vec3 col = light * f.t;

	//The meniscus line. Light hitting a steep oil-water boundary is thrown
	//sideways rather than forward, and the edge of every cell in a real
	//projection is a dark line for exactly that reason.
	col *= 1.0 - Meniscus * men;

	//The caustic. Added, because it IS light -- concentrated by the same
	//curvature that darkened the rim. Tinted halfway towards the local dye:
	//a caustic thrown through a red cell arrives red, but it has been through
	//less dye than the middle of the cell has.
	vec3 causticTint = mix( vec3( 1.0 ), f.t, 0.5 );
	col += Caustic * cst * luma( light ) * causticTint;

	col = max( col, vec3( 0.0 ) );

	//---- out through the gate -----------------------------------------
	//
	//Cover is how much of the light this pixel actually intercepts -- the dye
	//it has been through, or the rim treatment, whichever is doing more. Over
	//composites with it and Matte IS it, so it is worth the luma in every
	//mode rather than duplicated in two branches.
	float dyeCover = 1.0 - luma( f.t );
	float rimCover = max( Meniscus * men, min( Caustic * cst, 1.0 ) );
	float cover    = clamp( max( dyeCover, rimCover ), 0.0, 1.0 ) * gate;

	if( Mode == 3 )
	{
		//Matte: the lit oil over transparency. Over against nothing, so the
		//clip plays no part -- which makes this the one mode that renders
		//identically in the generator and the effect.
		//
		//Premultiplied, so where the oil covers nothing the pixel is
		//transparent AND black. A host that ignores alpha still gets the
		//black field somebody keying this would have asked for.
		fragColor = vec4( col * cover, cover );
	}
	else if( HasClip == 1 && Mode == 1 )
	{
		//Over: the lit oil sits on the clip.
		//Premultiplied destination, straight source: the standard over.
		fragColor = vec4( clip.rgb * ( 1.0 - cover ) + col * cover,
		                  clamp( clip.a * ( 1.0 - cover ) + cover, 0.0, 1.0 ) );
	}
	else
	{
		//Project, Colourise, and the generator. The picture IS the light that
		//got through, so the gate is the alpha -- and where the clip was
		//already transparent, so is this.
		float alpha = HasClip == 1 ? clip.a * gate : gate;
		fragColor   = vec4( col * gate, clamp( alpha, 0.0, 1.0 ) );
	}
}
)";

//---------------------------------------------------------------------------
// Pass 2: simmer. The one stateful thing in the plugin.
//---------------------------------------------------------------------------
static const char* const kSimmerPreamble = R"(#version 410 core

uniform sampler2D OilTexture;
uniform sampler2D FeedTexture;

uniform float Aspect;
uniform float Churn;
uniform float Grain;
uniform float BoilPhase;
uniform float Smear;
uniform float Persist;

in vec2 uv;
out vec4 fragColor;
)";

static const char* const kSimmerMain = R"(
void main()
{
	vec2 p = vec2( ( uv.x - 0.5 ) * 2.0 * Aspect, ( uv.y - 0.5 ) * 2.0 );

	//The direction the churn is pushing this point, as a unit-ish vector. The
	//warp field is what the oil is doing, so dragging the previous frame
	//along it is dragging it the way the oil went -- which is the difference
	//between a smear that looks like flow and one that looks like a lens
	//being wobbled.
	vec2 flow = warpPoint( p, 1.0, Grain, BoilPhase ) - p;

	vec2 offset = vec2( flow.x / ( 2.0 * Aspect ), flow.y * 0.5 ) * Smear;

	vec4 oil  = texture( OilTexture, uv );
	vec4 prev = texture( FeedTexture, clamp( uv - offset, vec2( 0.0 ), vec2( 1.0 ) ) );

	//A ceiling, not a sum. Persist is strictly below 1 and this is a max
	//against the live frame, so the feedback buffer can never hold anything
	//brighter than the brightest oil that has been through it -- the whole
	//class of runaway that a feedback loop with a gain of 1.02 gets you, at a
	//cost of one instruction.
	fragColor = max( oil, prev * Persist );
}
)";

//---------------------------------------------------------------------------
// Pass 3: composite.
//---------------------------------------------------------------------------
const char* const kCompositeShader = R"(#version 410 core

uniform sampler2D OilTexture;
uniform sampler2D FeedTexture;
uniform sampler2D SourceTexture;

//SourceTexture is the HOST's texture, which may be bigger than the picture
//that was drawn into it; the other two are ours and are exactly the picture.
//Hence one MaxUV here and not three.
uniform vec2 SourceMaxUV;

uniform float Simmer;
uniform float MixAmount;
uniform int HasClip;

in vec2 uv;
out vec4 fragColor;

void main()
{
	vec4 oil  = texture( OilTexture, uv );
	vec4 feed = texture( FeedTexture, uv );

	//At Simmer 0 this is `oil` exactly -- not nearly, exactly -- which is
	//what lets the whole feedback path be skipped on the frames that do not
	//use it without the picture changing on the frame it is switched on.
	vec4 lit = mix( oil, max( oil, feed ), Simmer );

	vec4 source = HasClip == 1 ? texture( SourceTexture, uv * SourceMaxUV ) : vec4( 0.0 );

	fragColor = mix( source, lit, MixAmount );
}
)";

//---------------------------------------------------------------------------
// The probe. Only `fltest --field` builds this.
//---------------------------------------------------------------------------
static const char* const kProbePreamble = R"(#version 410 core

uniform float Aspect;

uniform float Merge;
uniform float Density;
uniform float Rim;
uniform float Churn;
uniform float Grain;
uniform float BoilPhase;

uniform float Refraction;
uniform float Lamp;
uniform float Hotspot;
uniform float Temperature;
uniform float Gate;
uniform float GateSoft;

uniform vec2 ProbeSpan;
uniform vec2 ProbeSize;
uniform int ProbeSlot;

in vec2 uv;
out vec4 fragColor;
)";

static const char* const kProbeMain = R"(
void main()
{
	//One sample point per pixel, from the pixel's own centre. The harness
	//computes the identical point in C++ (Shaders.cpp: ProbePoint) -- if the
	//two ever disagreed the check would compare two different places and
	//report a difference that is not one.
	vec2 pixel = floor( gl_FragCoord.xy );
	vec2 t     = ( pixel + vec2( 0.5 ) ) / ProbeSize;
	vec2 p     = ( t * 2.0 - vec2( 1.0 ) ) * ProbeSpan;

	vec2 pw = warpPoint( p, Churn, Grain, BoilPhase );

	Field f    = fieldAt( pw, Merge, Density, Rim );
	vec2 bend  = bendAt( f, Refraction, Rim );
	float gate = gateAt( p, Gate, GateSoft );

	//From the nearest single boundary, not from the union's surface. See the
	//note on Field.dn in Oil.h: drawn from `d` this is one meniscus round the
	//outside of the whole pile, and a dozen cells come out as one flat
	//coloured shape with the light doing nothing anywhere inside it.
	float cst, men;
	rimProfiles( f.dn, Rim, cst, men );

	vec3 light = synthLamp( p, Lamp, Hotspot, Temperature );

	if( ProbeSlot == 0 )
		fragColor = vec4( f.d, f.g.x, f.g.y, gate );
	else if( ProbeSlot == 1 )
		fragColor = vec4( f.t, noise2( p.x * Grain, p.y * Grain ) );
	else if( ProbeSlot == 2 )
		fragColor = vec4( cst, men, bend.x, bend.y );
	else if( ProbeSlot == 3 )
		fragColor = vec4( light, 0.0 );
	else
		fragColor = vec4( pw.x, pw.y, f.dn, 0.0 );
}
)";

//---------------------------------------------------------------------------
// Assembly.
//---------------------------------------------------------------------------
std::string OilShaderSource()
{
	return std::string( kOilPreamble ) + kOilLibrary + kOilMain;
}

std::string SimmerShaderSource()
{
	return std::string( kSimmerPreamble ) + kOilLibrary + kSimmerMain;
}

std::string FieldProbeShaderSource()
{
	return std::string( kProbePreamble ) + kOilLibrary + kProbeMain;
}

void ProbePoint( int px, int py, int width, int height, float spanX, float spanY,
                 float& outX, float& outY )
{
	const float tx = ( static_cast< float >( px ) + 0.5f ) / static_cast< float >( width );
	const float ty = ( static_cast< float >( py ) + 0.5f ) / static_cast< float >( height );

	outX = ( tx * 2.0f - 1.0f ) * spanX;
	outY = ( ty * 2.0f - 1.0f ) * spanY;
}

} // namespace flenser
