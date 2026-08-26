/**
 * Flenser — browser demo.
 *
 * The shaders below are copied unedited from `source/Shaders.cpp`. The oil
 * pass and the simmer pass are ASSEMBLED here from the same pieces
 * `OilShaderSource()` and `SimmerShaderSource()` concatenate, so the oil
 * library in the middle of both is the same text the plugin compiles.
 * `demo/tools/check_shaders.py` proves it, character for character, and runs
 * in `tools/verify.sh`.
 *
 * `Controls.cpp` and the per-cell half of `Oil.cpp` are not here at all: they
 * are ported into `demo/oil.js`, which `demo/tools/check_cells.mjs` compares
 * against the plugin's own answers. The parameter declarations — names,
 * groups, order, elements and defaults — come from `Flenser.cpp`'s
 * constructor, and the presets from `Presets.h`.
 *
 * The one idea, before any of it: **the wheel is subtractive.** Each cell is a
 * dye filter, and light through two of them is multiplied twice — so cyan over
 * magenta is blue and not white. Almost every other blob effect adds, and
 * adding is what makes them look like lava lamps rather than like this. Put
 * Density up and watch two cells cross.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture } from './vendor/gl.js';
import {
  MAX_CELLS,
  cellsFromParam, sizeFromParam, variationFromParam, mergeFromParam,
  spreadFromParam, scatterFromParam, seedFromParam,
  speedFromParam, driftFromParam, spinFromParam, churnFromParam,
  grainFromParam, boilFromParam,
  densityFromParam, refractionFromParam, dispersionFromParam,
  meniscusFromParam, causticFromParam, rimFromParam,
  hueFromParam, hueSpreadFromParam, saturationFromParam,
  lampFromParam, hotspotFromParam, temperatureFromParam,
  gateFromParam, gateSoftFromParam, GATE_OFF,
  simmerFromParam, smearFromParam,
  PALETTE_NAMES, LAMP_MODE_NAMES,
  cellAt, setFreeRunningPhases,
} from './oil.js';

//===========================================================================
// The shaders. Copied from source/Shaders.cpp.
//===========================================================================

const VERTEX_SHADER = `#version 410 core

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
`;
const OIL_LIBRARY = `
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
	//is exactly the sort of difference \`--field\` reports and nobody can
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

			//\`h\` is the weight of the ACCUMULATED side, so the new cell's
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
	//and \`dn\` rather than \`d\`, so a cell lying over another one refracts
	//through its own edge as well as through the pile's outline.
	float lens = exp( -e * e );

	return refraction * lens * f.g;
}
`;
const OIL_PREAMBLE = `#version 410 core

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
`;
const OIL_MAIN = `
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
	//note on Field.dn in Oil.h: drawn from \`d\` this is one meniscus round the
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
	if( HasClip == 1 && Mode == 1 )
	{
		//Over: the lit oil sits on the clip. Cover is how much of the light
		//this pixel actually intercepts -- the dye it has been through, or
		//the rim treatment, whichever is doing more.
		float dyeCover = 1.0 - luma( f.t );
		float rimCover = max( Meniscus * men, min( Caustic * cst, 1.0 ) );
		float cover    = clamp( max( dyeCover, rimCover ), 0.0, 1.0 ) * gate;

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
`;
const SIMMER_PREAMBLE = `#version 410 core

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
`;
const SIMMER_MAIN = `
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
`;
const COMPOSITE_SHADER = `#version 410 core

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

	//At Simmer 0 this is \`oil\` exactly -- not nearly, exactly -- which is
	//what lets the whole feedback path be skipped on the frames that do not
	//use it without the picture changing on the frame it is switched on.
	vec4 lit = mix( oil, max( oil, feed ), Simmer );

	vec4 source = HasClip == 1 ? texture( SourceTexture, uv * SourceMaxUV ) : vec4( 0.0 );

	fragColor = mix( source, lit, MixAmount );
}
`;

const OIL_SHADER = OIL_PREAMBLE + OIL_LIBRARY + OIL_MAIN;
const SIMMER_SHADER = SIMMER_PREAMBLE + OIL_LIBRARY + SIMMER_MAIN;

//===========================================================================
// Presets, from source/Presets.h.
//===========================================================================

const PRESET_ORDER = [
  'cells', 'size', 'variation', 'merge', 'spread', 'scatter',
  'speed', 'drift', 'spin', 'churn', 'grain', 'boil',
  'density', 'refraction', 'dispersion', 'meniscus', 'caustic', 'rim',
  'palette', 'hue', 'hueSpread', 'saturation',
  'lamp', 'hotspot', 'temperature', 'gate', 'gateSoft',
];

function preset(name, values) {
  const out = { name, values: {} };
  PRESET_ORDER.forEach((id, i) => { out.values[id] = values[i]; });
  return out;
}

const PRESETS = [
  preset('Overhead Projector', [
    0.537, 0.677, 0.30, 0.810, 0.500, 0.20,
    0.067, 0.583, 0.540, 0.880, 0.346, 0.150,
    0.85, 0.686, 0.35, 0.55, 0.35, 0.533,
    0, 0.00, 0.85, 0.80,
    0.560, 0.55, 0.700, 0.653, 0.30,
  ]),
  preset('Optikinetics', [
    0.821, 0.473, 0.25, 0.523, 0.786, 0.10,
    0.133, 0.417, 0.600, 0.800, 0.506, 0.200,
    0.80, 0.564, 0.30, 0.50, 0.45, 0.373,
    0, 0.10, 0.90, 0.90,
    0.550, 0.20, 0.350, 1.000, 0.15,
  ]),
  preset('Alcohol Burn', [
    0.895, 0.354, 0.65, 0.667, 0.786, 0.55,
    0.400, 0.709, 0.500, 0.980, 0.654, 0.600,
    0.75, 0.807, 0.60, 0.65, 0.70, 0.254,
    0, 0.00, 1.00, 0.95,
    0.625, 0.30, 0.500, 0.875, 0.20,
  ]),
  preset('Ink in Water', [
    0.642, 0.586, 0.45, 0.926, 0.643, 0.40,
    0.100, 0.709, 0.480, 0.940, 0.250, 0.250,
    1.00, 0.443, 0.20, 0.40, 0.25, 0.694,
    1, 0.00, 1.00, 1.00,
    0.650, 0.25, 0.500, 1.000, 0.10,
  ]),
  preset('Sodium', [
    0.716, 0.473, 0.55, 0.667, 0.643, 0.35,
    0.133, 0.583, 0.520, 0.900, 0.346, 0.200,
    0.95, 0.564, 0.15, 0.60, 0.55, 0.533,
    2, 0.00, 1.00, 0.90,
    0.700, 0.75, 0.800, 0.740, 0.45,
  ]),
  preset('Clear Water', [
    0.821, 0.473, 0.50, 0.523, 0.786, 0.30,
    0.233, 0.583, 0.500, 0.860, 0.506, 0.350,
    0.30, 0.807, 0.75, 0.70, 0.80, 0.373,
    5, 0.00, 0.00, 0.00,
    0.500, 0.15, 0.450, 1.000, 0.10,
  ]),
  preset('Beading', [
    0.953, 0.204, 0.80, 0.000, 0.929, 0.75,
    0.267, 0.834, 0.560, 0.700, 0.830, 0.400,
    1.00, 0.443, 0.25, 0.35, 0.60, 0.150,
    3, 0.00, 1.00, 1.00,
    0.550, 0.10, 0.500, 1.000, 0.05,
  ]),
  preset('Slow Bloom', [
    0.284, 0.881, 0.20, 0.926, 0.357, 0.15,
    0.047, 0.709, 0.515, 0.930, 0.150, 0.100,
    0.70, 0.686, 0.40, 0.30, 0.30, 0.850,
    4, 0.55, 0.60, 0.75,
    0.600, 0.40, 0.650, 0.875, 0.60,
  ]),
];

//===========================================================================
// The renderer. The same three passes ProcessOpenGL runs, in the same order.
//===========================================================================

class FlenserRenderer {
  constructor(gl, quad) {
    this.gl = gl;
    this.quad = quad;

    this.oilProgram = new Program(gl, VERTEX_SHADER, OIL_SHADER, 'oil');
    this.simmerProgram = new Program(gl, VERTEX_SHADER, SIMMER_SHADER, 'simmer');
    this.compositeProgram = new Program(gl, VERTEX_SHADER, COMPOSITE_SHADER, 'composite');

    // RGBA16F, as the plugin allocates them. The caustic is deliberately
    // allowed past 1 -- it is light that has been concentrated -- and an
    // 8-bit intermediate clips it flat before Mix can pull it back.
    this.oilBuffer = new PassBuffer(gl, { filter: 'linear' });
    this.feed = [new PassBuffer(gl, { filter: 'linear' }), new PassBuffer(gl, { filter: 'linear' })];
    this.feedIndex = 0;
    this.feedPrimed = false;

    // Flat arrays, filled once per frame from cellAt, exactly as the plugin
    // fills them before handing them to the GPU as uniforms.
    this.cellPos = new Float32Array(MAX_CELLS * 3);
    this.cellDye = new Float32Array(MAX_CELLS * 3);

    this.lastSize = '';
  }

  render({ input, params, width, height, time, variant }) {
    const gl = this.gl;
    const value = (id) => params.get(id);
    const overInput = variant !== 'source';

    const aspect = width / height;

    //---- the wheel, resolved once for this frame ------------------------
    const wheel = {
      cells: cellsFromParam(value('cells')),
      size: sizeFromParam(value('size')),
      variation: variationFromParam(value('variation')),
      merge: mergeFromParam(value('merge')),
      spread: spreadFromParam(value('spread')),
      scatter: scatterFromParam(value('scatter')),
      seed: seedFromParam(value('seed')),
      speed: speedFromParam(value('speed')),
      drift: driftFromParam(value('drift')),
      spin: spinFromParam(value('spin')),
      palette: Math.round(value('palette')),
      hue: hueFromParam(value('hue')),
      hueSpread: hueSpreadFromParam(value('hueSpread')),
      saturation: saturationFromParam(value('saturation')),
      time,
    };

    // As the OpenFX build does it, not as the FFGL build does — the same
    // trade, and for the same reason, as the boil phase below.
    setFreeRunningPhases(wheel);

    const cellCount = Math.min(wheel.cells, MAX_CELLS);
    for (let i = 0; i < cellCount; ++i) {
      const cell = cellAt(i, wheel);
      this.cellPos[i * 3 + 0] = cell[0];
      this.cellPos[i * 3 + 1] = cell[1];
      this.cellPos[i * 3 + 2] = cell[2];
      this.cellDye[i * 3 + 0] = cell[3];
      this.cellDye[i * 3 + 1] = cell[4];
      this.cellDye[i * 3 + 2] = cell[5];
    }

    const churn = churnFromParam(value('churn'));
    const grain = grainFromParam(value('grain'));
    const boil = boilFromParam(value('boil'));

    // `time * boil` here, as the OpenFX build does it and not as the FFGL
    // build does. This page has no host clock to integrate against and it is
    // scrubbable in exactly the way an OFX host is, so the deterministic form
    // is the honest one. The difference is noted at the foot of the page.
    const boilPhase = time * boil;

    const simmer = simmerFromParam(value('simmer'));
    const smear = smearFromParam(value('smear'));

    //---- buffers --------------------------------------------------------
    const size = `${width}x${height}`;
    if (size !== this.lastSize) {
      this.feedPrimed = false;
      this.feedIndex = 0;
      this.lastSize = size;
    }
    this.oilBuffer.ensure(width, height, gl.RGBA16F);
    this.feed[0].ensure(width, height, gl.RGBA16F);
    this.feed[1].ensure(width, height, gl.RGBA16F);

    gl.disable(gl.BLEND);

    //---- 1. the oil -----------------------------------------------------
    this.oilBuffer.bind();
    const oil = this.oilProgram.use();
    bindTexture(gl, 0, input.texture);
    oil.setSampler('InputTexture', 0);
    oil.set('MaxUV', 1, 1);
    oil.set('HalfTexel', 0.5 / width, 0.5 / height);
    oil.set('Aspect', aspect);

    oil.setInt('CellCount', cellCount);
    if (cellCount > 0) {
      // setArray and not set(): a `set` with one value issues glUniform1f, and
      // on a vec3 array that is rejected as a size mismatch and the uniform
      // keeps whatever it held before -- which here is every cell at the origin
      // at zero radius, i.e. an empty wheel that looks like the plugin is off.
      oil.setArray('CellPos', this.cellPos.subarray(0, cellCount * 3), 3);
      oil.setArray('CellDye', this.cellDye.subarray(0, cellCount * 3), 3);
    }

    oil.set('Merge', wheel.merge);
    oil.set('Density', densityFromParam(value('density')));
    oil.set('Rim', rimFromParam(value('rim')));
    oil.set('Churn', churn);
    oil.set('Grain', grain);
    oil.set('BoilPhase', boilPhase);

    oil.set('Refraction', refractionFromParam(value('refraction')));
    oil.set('Dispersion', dispersionFromParam(value('dispersion')));
    oil.set('Meniscus', meniscusFromParam(value('meniscus')));
    oil.set('Caustic', causticFromParam(value('caustic')));

    oil.set('Lamp', lampFromParam(value('lamp')));
    oil.set('Hotspot', hotspotFromParam(value('hotspot')));
    oil.set('Temperature', temperatureFromParam(value('temperature')));
    oil.set('Gate', gateFromParam(value('gate')));
    oil.set('GateSoft', gateSoftFromParam(value('gateSoft')));

    oil.setInt('Mode', Math.round(value('mode')));
    oil.setInt('HasClip', overInput ? 1 : 0);
    this.quad.draw();

    //---- 2. simmer, skipped when it is off ------------------------------
    if (simmer > 0) {
      const from = this.feedIndex;
      const to = 1 - this.feedIndex;

      this.feed[to].bind();
      const pass = this.simmerProgram.use();
      bindTexture(gl, 0, this.oilBuffer.texture);
      bindTexture(gl, 1, this.feed[from].texture);
      pass.setSampler('OilTexture', 0);
      pass.setSampler('FeedTexture', 1);
      pass.set('Aspect', aspect);
      pass.set('Churn', churn);
      pass.set('Grain', grain);
      pass.set('BoilPhase', boilPhase);
      pass.set('Smear', smear);
      // A buffer that has never been written holds nothing, and reading it as
      // history would put one frame of black under the oil on the frame the
      // control is switched on.
      pass.set('Persist', this.feedPrimed ? 0.94 * simmer : 0);
      pass.setInt('CellCount', 0);
      this.quad.draw();

      this.feedIndex = to;
      this.feedPrimed = true;
    }

    //---- 3. composite, to the canvas ------------------------------------
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, width, height);

    const composite = this.compositeProgram.use();
    bindTexture(gl, 0, this.oilBuffer.texture);
    bindTexture(gl, 1, this.feed[this.feedIndex].texture);
    bindTexture(gl, 2, overInput ? input.texture : this.oilBuffer.texture);
    composite.setSampler('OilTexture', 0);
    composite.setSampler('FeedTexture', 1);
    composite.setSampler('SourceTexture', 2);
    composite.set('SourceMaxUV', 1, 1);
    composite.set('Simmer', simmer);
    composite.set('MixAmount', value('mix'));
    composite.setInt('HasClip', overInput ? 1 : 0);
    this.quad.draw();
  }
}

//===========================================================================

const gateDisplay = (v) => {
  const g = gateFromParam(v);
  return g >= GATE_OFF ? 'off' : g.toFixed(2);
};

mountDemo({
  name: 'Flenser',
  pluginId: 'FL01',
  tagline:
    'Oil, water, alcohol and dye between two watch glasses on an overhead projector. The cells are dye filters, so where they overlap they multiply — cyan over magenta is blue, not white.',
  repo: 'https://github.com/stoatworks-labs/flenser',
  needFloat: true,
  showBackdrop: true,
  presets: PRESETS,

  variants: {
    label: 'Plugin',
    default: 'lamp',
    options: [
      { id: 'lamp', name: 'Flenser Lamp (effect)', hint: 'FF_EFFECT — the clip goes where the projector\'s lamp was, and you look through the oil at it.' },
      { id: 'source', name: 'Flenser (source)', hint: 'FF_SOURCE — the wheel with the plugin\'s own lamp behind it; takes no input.' },
    ],
  },

  // Colour bars first: the dye stack is the thing worth seeing before
  // anything else, and a saturated primary under a complementary dye going
  // black is the clearest possible statement of what subtractive means. Detail
  // second, because refraction is a displacement and needs something with a
  // known shape to displace.
  sources: ['bars', 'detail', 'grid', 'scene', 'spot', 'ramp', 'alpha'],

  differences: [
    'The boil phase here is time × Boil, which is how the OpenFX build does it. The Resolume build INTEGRATES the rate instead, so that nudging Boil live changes what happens next rather than rescaling the whole history and jumping the field somewhere else. Scrub-ability was the trade, and this page is scrubbable.',
    'Speed and Spin are the same trade. Here, and in the OpenFX build, the orbit and rotation phases are time × rate. The Resolume build ANCHORS them, carrying the phase forward whenever the rate changes, because otherwise moving either control an hour into a show rescales the whole history and teleports the wheel — the two orbit frequencies never repeat, so there is no old position to land back on.',
    'Simmer is a feedback buffer, and it exists in the Resolume build and on this page but NOT in the OpenFX build — a host that renders frames out of order cannot have one and still match its own preview.',
    'A browser tab that loses focus throttles its animation frames. Everything except Simmer is a pure function of the clock, so the picture is unaffected; the simmer trail is shorter in real time than it would be in a host.',
    'Preset is an option parameter in the plugin, with Custom as element 0 and a slider edit dropping back to it. Here the same eight presets are in the panel header instead, from the plugin\'s own table.',
    'The plugin fetches the clip through the host\'s texture, which can be bigger than the picture — hence MaxUV. Here the clip is generated at exactly the canvas size, so MaxUV is 1 and that path is never exercised.',
  ],

  params: [
    //---- Wheel ------------------------------------------------------------
    {
      id: 'cells', name: 'Cells', type: 'standard', default: 0.86, group: 'Wheel',
      display: (v) => `${cellsFromParam(v)}`,
      hint: 'How many cells of oil are on the glass, 1 to 48. This is the one control that is linear in render cost — the field walks every cell at every pixel.',
    },
    {
      id: 'size', name: 'Size', type: 'standard', default: 0.62, group: 'Wheel',
      display: (v) => sizeFromParam(v).toFixed(3),
      hint: 'Average cell radius, where 1 is half the short edge of the frame.',
    },
    {
      id: 'variation', name: 'Variation', type: 'standard', default: 0.40, group: 'Wheel',
      display: (v) => variationFromParam(v).toFixed(2),
      hint: 'How much the cells differ in size. At 1 the smallest is a tenth of the largest, which is roughly what a dish that has been squeezed looks like.',
    },
    {
      id: 'merge', name: 'Merge', type: 'standard', default: 0.78, group: 'Wheel',
      display: (v) => (mergeFromParam(v) === 0 ? 'off' : mergeFromParam(v).toFixed(3)),
      hint: 'The fillet where two cells meet. At 0 they stay separate beads and pass through one another; as it rises they pull into each other the way oil in water actually does.',
    },
    {
      id: 'spread', name: 'Spread', type: 'standard', default: 0.85, group: 'Wheel',
      display: (v) => spreadFromParam(v).toFixed(2),
      hint: 'How wide the cells are packed. Past 1 the outer ones sit beyond the frame, which is the normal way to run it — a real wheel is bigger than the gate.',
    },
    {
      id: 'scatter', name: 'Scatter', type: 'standard', default: 0.30, group: 'Wheel',
      display: (v) => scatterFromParam(v).toFixed(2),
      hint: 'At 0 the cells sit on a golden-angle spiral, which is what a dish full of equal cells pressing on each other settles into. At 1 they are a hashed swarm — a dish that has just been rocked.',
    },
    {
      id: 'seed', name: 'Seed', type: 'standard', default: 0.0, group: 'Wheel',
      display: (v) => `${seedFromParam(v)}`,
      hint: 'A different wheel.',
    },

    //---- Motion -----------------------------------------------------------
    {
      id: 'speed', name: 'Speed', type: 'standard', default: 0.14, group: 'Motion',
      display: (v) => `${speedFromParam(v).toFixed(2)} Hz`,
      hint: 'Cycles per second. Every cell travels a closed orbit on two incommensurate frequencies, so the wheel never comes back to where it was.',
    },
    {
      id: 'drift', name: 'Drift', type: 'standard', default: 0.60, group: 'Motion',
      display: (v) => driftFromParam(v).toFixed(3),
      hint: 'How far each cell travels on its own convection orbit.',
    },
    {
      id: 'spin', name: 'Spin', type: 'standard', default: 0.50, group: 'Motion',
      display: (v) => `${spinFromParam(v) >= 0 ? '+' : ''}${spinFromParam(v).toFixed(3)}`,
      hint: 'The whole wheel turning in its holder. Bipolar: 0.5 is still.',
    },
    {
      id: 'churn', name: 'Churn', type: 'standard', default: 0.90, group: 'Motion',
      display: (v) => (churnFromParam(v) === 0 ? 'off' : churnFromParam(v).toFixed(3)),
      hint: 'How far the noise field deforms the oil, as a fraction of its own wavelength. A fraction and not a distance: an absolute displacement large against the feature size folds the plane, and every cell comes out as a nest of contour lines rather than as a shape.',
    },
    {
      id: 'grain', name: 'Grain', type: 'standard', default: 0.70, group: 'Motion',
      display: (v) => grainFromParam(v).toFixed(2),
      hint: 'How fine that noise is. Low is a slow swell that moves whole cells; high is the boiling texture a projector gets after a few minutes.',
    },
    {
      id: 'boil', name: 'Boil', type: 'standard', default: 0.20, group: 'Motion',
      display: (v) => `${boilFromParam(v).toFixed(2)} Hz`,
      hint: 'How fast the noise moves. 0 freezes it, which leaves the cells deformed but still.',
    },

    //---- Optics -----------------------------------------------------------
    {
      id: 'density', name: 'Density', type: 'standard', default: 0.85, group: 'Optics',
      display: (v) => densityFromParam(v).toFixed(2),
      hint: 'How much light a cell\'s dye absorbs. This is the control that makes the effect subtractive: overlapping cells MULTIPLY, so two crossing dyes go darker than either.',
    },
    {
      id: 'refraction', name: 'Refraction', type: 'standard', default: 0.62, group: 'Optics',
      display: (v) => (refractionFromParam(v) === 0 ? 'off' : refractionFromParam(v).toFixed(4)),
      hint: 'How far the meniscus displaces what is behind it. An edge band, not a whole-cell distortion: the middle of a cell is flat oil between two flat glasses and bends light by almost nothing.',
    },
    {
      id: 'dispersion', name: 'Dispersion', type: 'standard', default: 0.30, group: 'Optics',
      display: (v) => dispersionFromParam(v).toFixed(2),
      hint: 'Red and blue bent by different amounts — the colour fringe a thick edge of oil makes. Green is left alone, so the picture does not appear to move as this opens.',
    },
    {
      id: 'meniscus', name: 'Meniscus', type: 'standard', default: 0.50, group: 'Optics',
      display: (v) => meniscusFromParam(v).toFixed(2),
      hint: 'The dark line at a cell\'s edge. Light hitting a steep oil-water boundary is thrown sideways instead of forward, and every cell edge in a real projection is dark for that reason.',
    },
    {
      id: 'caustic', name: 'Caustic', type: 'standard', default: 0.40, group: 'Optics',
      display: (v) => causticFromParam(v).toFixed(2),
      hint: 'The bright line just inside it, where the same curvature concentrates the light. Over 1 on purpose — a caustic genuinely is brighter than the lamp.',
    },
    {
      id: 'rim', name: 'Rim', type: 'standard', default: 0.38, group: 'Optics',
      display: (v) => rimFromParam(v).toFixed(4),
      hint: 'How wide the rim treatment is. At the bottom of its travel it is about a pixel, which is a projector in sharp focus.',
    },

    //---- Colour -----------------------------------------------------------
    {
      id: 'palette', name: 'Palette', type: 'option', default: 0, group: 'Colour',
      elements: PALETTE_NAMES,
      hint: 'The dye set. Aniline is the four bottles in a food-colouring pack; Ink is the subtractive primaries and overlaps most cleanly; Sodium is the hot end only.',
    },
    {
      id: 'hue', name: 'Hue', type: 'standard', default: 0.0, group: 'Colour',
      display: (v) => `${(hueFromParam(v) * 360).toFixed(0)}°`,
      hint: 'Rotates the whole palette round the wheel.',
    },
    {
      id: 'hueSpread', name: 'Hue Spread', type: 'standard', default: 0.80, group: 'Colour',
      display: (v) => hueSpreadFromParam(v).toFixed(2),
      hint: 'How far apart the palette\'s dyes sit. At 0 every cell is the same colour, which is a wheel charged with one bottle.',
    },
    {
      id: 'saturation', name: 'Saturation', type: 'standard', default: 0.85, group: 'Colour',
      display: (v) => saturationFromParam(v).toFixed(2),
      hint: 'How strong the dyes are. At 0 the wheel is clear water and every mark on the screen is refraction, a caustic or a meniscus line.',
    },

    //---- Lamp -------------------------------------------------------------
    {
      id: 'lamp', name: 'Lamp', type: 'standard', default: 0.62, group: 'Lamp',
      display: (v) => `×${lampFromParam(v).toFixed(2)}`,
      hint: 'Lamp brightness; 0.5 is unity. Over 1 because a dense dye passes maybe a fifth of the light, and needing to push past unity to get a picture back is the accurate behaviour.',
    },
    {
      id: 'hotspot', name: 'Hotspot', type: 'standard', default: 0.20, group: 'Lamp',
      display: (v) => hotspotFromParam(v).toFixed(2),
      hint: 'An overhead projector\'s condenser is a Fresnel lens and its hot spot is the first thing anybody notices about the format.',
    },
    {
      id: 'temperature', name: 'Temperature', type: 'standard', default: 0.58, group: 'Lamp',
      display: (v) => `${temperatureFromParam(v) >= 0 ? '+' : ''}${temperatureFromParam(v).toFixed(2)}`,
      hint: 'Bipolar. Below 0.5 is the cold blue-white of a metal halide head; above it is the amber of a tungsten overhead that has been dimmed.',
    },
    {
      id: 'gate', name: 'Gate', type: 'standard', default: 1.0, group: 'Lamp',
      display: gateDisplay,
      hint: 'The round gate. Below 1 the disc and its edge are the picture, which is an overhead projector with a clock glass on it. At the very top it is off, and off is off — a gate that merely got large would still be cutting the corners off a wide canvas at its maximum.',
    },
    {
      id: 'gateSoft', name: 'Gate Soft', type: 'standard', default: 0.20, group: 'Lamp',
      display: (v) => gateSoftFromParam(v).toFixed(2),
      hint: 'How soft the gate\'s edge is. At 0 it is the hard circle of a projector in focus.',
    },

    //---- Simmer -----------------------------------------------------------
    {
      id: 'simmer', name: 'Simmer', type: 'standard', default: 0.0, group: 'Simmer',
      display: (v) => (simmerFromParam(v) === 0 ? 'off' : simmerFromParam(v).toFixed(2)),
      hint: 'The one part of this plugin that remembers anything: the previous frame, dragged along the churn and carried forward under a ceiling. It cannot run away — the buffer is a max against the live frame with a persistence below 1. Not in the OpenFX build.',
    },
    {
      id: 'smear', name: 'Smear', type: 'standard', default: 0.45, group: 'Simmer',
      display: (v) => smearFromParam(v).toFixed(3),
      hint: 'How far a carried-forward frame is dragged along the flow before it is blended back. Does nothing with Simmer at 0.',
    },

    //---- Output -----------------------------------------------------------
    {
      id: 'mode', name: 'Mode', type: 'option', default: 0, group: 'Output',
      elements: LAMP_MODE_NAMES,
      hint: 'Project puts the clip where the lamp was and looks through the oil at it. Over lights the oil itself and lays it on the clip. Colourise takes only the clip\'s brightness and lets the dye supply all the colour.',
    },
    {
      id: 'mix', name: 'Mix', type: 'standard', default: 1.0, group: 'Output',
      display: (v) => v.toFixed(2),
      hint: 'Dry/wet against the untouched clip.',
    },
  ],

  createRenderer: (gl, quad) => new FlenserRenderer(gl, quad),
});
