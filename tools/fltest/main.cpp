/**
	fltest -- render Flenser offline, and check what the oil is doing.

	Where a cell of oil is, how big it is and what colour it dyes are facts,
	not opinions. So is the value of the merged distance field at a point, the
	direction of its surface, and the product of the dyes above it. All of
	that is a pure function of the wheel and a position -- and it exists
	TWICE: once in `Oil.cpp`, which this harness and the OpenFX build call,
	and once in GLSL, which is what the GPU runs. `--field` runs both and
	compares them.

	The trick that makes that possible is in `Shaders.h`: the GLSL oil library
	is a *fragment* rather than a shader, and the oil pass, the simmer pass
	and this harness's probe are all assembled around the same string. So what
	is being checked is the text the plugin actually runs. A probe with its
	own transcription of the library would agree with itself perfectly and
	prove nothing.

		fltest --out /tmp/frame.png     a picture, on a moving test card
		fltest --source --out f.png     the generator instead of the effect
		fltest --list                   every parameter, and what it resolves to
		fltest --cells                  the wheel as JSON, for the demo checker
		fltest --field                  GLSL against C++, over the whole space
		fltest --null                   a clear wheel does not touch the clip
		fltest --continuity             moving Speed or Spin does not move the picture
		fltest --matte                  the cutout mode, in both builds
		fltest --presets                every preset survives every host
		fltest --card /tmp/card.png     the test card on its own
		fltest --bench                  the render cost
		fltest --pipe                   raw frames in, raw frames out

	**`--null` is the invariant that matters.** With no dye, no refraction and
	no rim, the effect build is a piece of clear glass: the picture has to
	come out exactly as it went in, to 0/255. Every intermediate buffer, the
	half-texel inset, the MaxUV handling and the composite all sit on that
	path, and an effect that quietly lifts, tints or softens footage when it
	is turned down is an effect that has to be switched off between cues --
	which in practice means an effect nobody uses.

	`--script` is a plain text file of `frame  Parameter Name  value` lines.
	Values are held before the first key and after the last, and linearly
	interpolated between. The format is identical to agtest's, octest's and
	phtest's on purpose, so one build.py can film any of them.

	`--pipe` takes the fleet's frame format:

		ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - \
		  | fltest --pipe --width 1920 --height 1080 [--script cues.txt] \
		  | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -i - out.mov
*/

#include "Controls.h"
#include "Flenser.h"
#include "Hash.h"
#include "Oil.h"
#include "Presets.h"
#include "Shaders.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace flenser;

namespace
{
//---------------------------------------------------------------------------
/// How far the GLSL and the C++ are allowed to disagree.
///
/// Not zero, and it cannot be. The GLSL specification allows three units in
/// the last place for `exp` and gives `sin` and `cos` no accuracy requirement
/// at all, so the rim profiles and the lamp falloff will differ from libm's
/// in the last few bits on any driver, and a check that insisted on equality
/// would fail everywhere. What this tolerance catches is a *drifted
/// constant* -- a fade polynomial of 5 against 6, an octave ratio of 2.0
/// against 2.03, a hash multiplier with a digit wrong -- which misses by
/// percent, not by 1e-6.
constexpr float kFieldTolerance = 5.0e-4f;

/// Below this, a comparison is judged in absolute terms; above it, relative.
///
/// The distance field runs from about -1 inside a big cell to 1e6 where the
/// wheel is empty, and an absolute tolerance across five orders of magnitude
/// is either useless at the bottom or vacuous at the top.
constexpr float kRelativeAbove = 1.0f;

/// How far to step when asking whether a point is sitting on a discontinuity.
///
/// With Merge at zero the field is a plain `min`, whose VALUE is continuous
/// but whose gradient jumps where two cells are equidistant. At such a point
/// the two implementations are entitled to pick different cells for a
/// difference of one bit in the argument, and no tolerance can absorb that --
/// the whole point of a hard minimum is that the choice flips.
///
/// So the gradient is sampled either side and, where it swings, the
/// comparison is counted and skipped rather than failed. The count is
/// printed: a run where a large fraction went that way would mean the sample
/// points had been chosen badly and the check was measuring less than it
/// looks like it is.
constexpr float kGuardStep = 3.0e-4f;

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );//filter: none
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );//bit depth
	ihdr.push_back( 6 );//truecolour with alpha
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The test card.
//
// Not meant to look nice. Each part of it exercises one thing this plugin
// does to a picture:
//
//   - concentric rings:          Refraction, which is a displacement and is
//                                only readable against something with a known
//                                shape. A ring passing under a cell's rim
//                                kinks visibly and a ramp does not.
//   - six saturated colour bars: the dye stack, which MULTIPLIES -- a red bar
//                                under a cyan cell has to go black, and that
//                                is the single clearest picture of the
//                                difference between this and an additive
//                                blob effect
//   - a fine checkerboard:       Dispersion, whose fringe needs a hard edge,
//                                and any accidental softening on the null
//                                path
//   - a smooth luminance ramp:   the Lamp group, all of which are
//                                multiplications and none of which do
//                                anything visible to flat white
//   - a bright disc, moving:     the caustic and the hot spot, and the only
//                                thing in the frame bright enough to make a
//                                rim read at all
//   - a mid-grey surround:       so that a dye can darken as well as tint;
//                                on black every subtractive control is dead
//
// It MOVES, mildly. This plugin's picture changes on its own, so a still card
// would not hide a broken build the way it would in a trail plugin -- but
// tools/sweep.py compares frames rendered with one control moved, and a
// moving card keeps that comparison honest about the temporal controls too.
//---------------------------------------------------------------------------
std::vector< unsigned char > buildCard( int width, int height, int frame )
{
	std::vector< unsigned char > card( static_cast< size_t >( width ) * height * 4, 0 );

	const float t  = static_cast< float >( frame ) / 60.0f;
	const float cx = 0.5f + 0.22f * std::sin( 6.2831853f * 0.13f * t );
	const float cy = 0.5f + 0.17f * std::sin( 6.2831853f * 0.19f * t + 1.1f );

	auto put = [ & ]( int x, int y, float r, float g, float b, float a ) {
		const size_t i  = ( static_cast< size_t >( y ) * width + x ) * 4;
		auto toByte     = []( float v ) {
            return static_cast< unsigned char >(
                std::lround( std::min( std::max( v, 0.0f ), 1.0f ) * 255.0f ) );
		};
		card[ i + 0 ] = toByte( r );
		card[ i + 1 ] = toByte( g );
		card[ i + 2 ] = toByte( b );
		card[ i + 3 ] = toByte( a );
	};

	const float bars[ 6 ][ 3 ] = {
		{ 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f },
		{ 0.0f, 1.0f, 1.0f }, { 1.0f, 0.0f, 1.0f }, { 1.0f, 1.0f, 0.0f }
	};

	for( int y = 0; y < height; ++y )
	{
		const float v = ( static_cast< float >( y ) + 0.5f ) / height;

		for( int x = 0; x < width; ++x )
		{
			const float u = ( static_cast< float >( x ) + 0.5f ) / width;

			//The mid-grey surround.
			float r = 0.32f, g = 0.32f, b = 0.32f;

			//Concentric rings over the whole frame, faint, so they survive
			//being drawn over.
			const float rx   = ( u - 0.5f ) * 2.0f * ( float( width ) / float( height ) );
			const float ry   = ( v - 0.5f ) * 2.0f;
			const float ring = std::sin( 6.2831853f * 9.0f * std::sqrt( rx * rx + ry * ry ) );
			r += 0.10f * ring;
			g += 0.10f * ring;
			b += 0.10f * ring;

			//Colour bars across the top fifth.
			if( v < 0.20f )
			{
				const int bar = std::min( 5, static_cast< int >( u * 6.0f ) );
				r             = bars[ bar ][ 0 ];
				g             = bars[ bar ][ 1 ];
				b             = bars[ bar ][ 2 ];
			}
			//The luminance ramp across the bottom fifth.
			else if( v > 0.80f )
			{
				r = g = b = u;
			}
			//A fine checkerboard down the left quarter of the middle band.
			else if( u < 0.25f )
			{
				const int cell = ( ( x / 4 ) + ( y / 4 ) ) & 1;
				r = g = b = cell ? 0.92f : 0.06f;
			}

			//The moving disc.
			const float dx = ( u - cx ) * ( float( width ) / float( height ) );
			const float dy = v - cy;
			const float d  = std::sqrt( dx * dx + dy * dy );
			if( d < 0.13f )
			{
				const float k = 1.0f - std::min( d / 0.13f, 1.0f );
				r             = std::max( r, k );
				g             = std::max( g, k );
				b             = std::max( b, k * 0.9f );
			}

			put( x, y, r, g, b, 1.0f );
		}
	}

	return card;
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	//Accelerated first; fall back so the harness still runs somewhere without
	//a GPU, where it will at least prove the shaders compile.
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, const unsigned char* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

std::vector< unsigned char > flipRows( const std::vector< unsigned char >& image, int width, int height )
{
	std::vector< unsigned char > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + static_cast< size_t >( y ) * stride,
		             image.data() + static_cast< size_t >( height - 1 - y ) * stride, stride );
	return flipped;
}

std::vector< unsigned char > readBackRaw( GLuint fbo, int width, int height )
{
	std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
	return pixels;
}

//---------------------------------------------------------------------------
// Shader compilation, for the probe. The plugin uses ffglex for this; the
// probe cannot, because ffglex::FFGLShader insists on a vertex shader with
// the SDK's attribute layout and its own uniform helpers, and the probe needs
// neither.
//---------------------------------------------------------------------------
GLuint compileStage( GLenum type, const std::string& source, std::string& error )
{
	const GLuint shader   = glCreateShader( type );
	const char* const ptr = source.c_str();
	glShaderSource( shader, 1, &ptr, nullptr );
	glCompileShader( shader );

	GLint compiled = GL_FALSE;
	glGetShaderiv( shader, GL_COMPILE_STATUS, &compiled );
	if( compiled == GL_TRUE )
		return shader;

	GLint length = 0;
	glGetShaderiv( shader, GL_INFO_LOG_LENGTH, &length );
	std::string log( static_cast< size_t >( std::max( length, 1 ) ), '\0' );
	glGetShaderInfoLog( shader, length, nullptr, log.data() );
	error = log;
	glDeleteShader( shader );
	return 0;
}

GLuint buildProbeProgram( std::string& error )
{
	static const char* const vertexSource = R"(#version 410 core
layout( location = 0 ) in vec4 vPosition;
void main() { gl_Position = vPosition; }
)";

	const GLuint vertex = compileStage( GL_VERTEX_SHADER, vertexSource, error );
	if( vertex == 0 )
		return 0;

	const GLuint fragment = compileStage( GL_FRAGMENT_SHADER, FieldProbeShaderSource(), error );
	if( fragment == 0 )
	{
		glDeleteShader( vertex );
		return 0;
	}

	const GLuint program = glCreateProgram();
	glAttachShader( program, vertex );
	glAttachShader( program, fragment );
	glLinkProgram( program );
	glDeleteShader( vertex );
	glDeleteShader( fragment );

	GLint linked = GL_FALSE;
	glGetProgramiv( program, GL_LINK_STATUS, &linked );
	if( linked == GL_TRUE )
		return program;

	GLint length = 0;
	glGetProgramiv( program, GL_INFO_LOG_LENGTH, &length );
	std::string log( static_cast< size_t >( std::max( length, 1 ) ), '\0' );
	glGetProgramInfoLog( program, length, nullptr, log.data() );
	error = log;
	glDeleteProgram( program );
	return 0;
}

//---------------------------------------------------------------------------
// Parameters, by display name.
//---------------------------------------------------------------------------
struct NamedParameter
{
	unsigned int index;
	std::string name;
	float value;
};

std::vector< NamedParameter > listParameters( FlenserPlugin& plugin )
{
	std::vector< NamedParameter > out;
	for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
	{
		const char* name = plugin.GetParamName( i );
		out.push_back( { i, name ? name : "", plugin.GetFloatParameter( i ) } );
	}
	return out;
}

bool applySetting( FlenserPlugin& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=value, got \"" + assignment + "\"";
		return false;
	}

	const std::string name = assignment.substr( 0, equals );
	const float value      = std::strtof( assignment.substr( equals + 1 ).c_str(), nullptr );

	for( const NamedParameter& p : listParameters( plugin ) )
	{
		if( p.name == name )
		{
			plugin.SetFloatParameter( p.index, value );
			return true;
		}
	}

	error = "no parameter called \"" + name + "\"";
	return false;
}

int presetIndexByName( FlenserPlugin& plugin, const std::string& name )
{
	for( int i = 0; i < presets::kCount; ++i )
		if( name == presets::kPresets[ i ].name )
			return i + 1;
	(void)plugin;
	return -1;
}

unsigned int paramIndexByName( FlenserPlugin& plugin, const char* name )
{
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.name == name )
			return p.index;
	return 0xffffffffu;
}

//---------------------------------------------------------------------------
// The wheel, as text and as JSON.
//---------------------------------------------------------------------------
void printResolved( FlenserPlugin& plugin )
{
	const Wheel w = plugin.ResolveWheel( 16.0f / 9.0f );

	std::printf( "\nresolved for a 16:9 frame:\n" );
	std::printf( "  cells       %d\n", w.cells );
	std::printf( "  size        %.4f wheel units\n", w.size );
	std::printf( "  variation   %.4f\n", w.variation );
	std::printf( "  merge       %.4f\n", w.merge );
	std::printf( "  spread      %.4f\n", w.spread );
	std::printf( "  scatter     %.4f\n", w.scatter );
	std::printf( "  seed        %d\n", w.seed );
	std::printf( "  speed       %.4f cycles/s\n", w.speed );
	std::printf( "  drift       %.4f wheel units\n", w.drift );
	std::printf( "  spin        %+.4f turns/s\n", w.spin );
	std::printf( "  churn       %.4f wheel units\n", w.churn );
	std::printf( "  grain       %.4f cells across\n", w.grain );
	std::printf( "  boil        %.4f cycles/s\n", w.boil );
	std::printf( "  density     %.4f\n", w.density );
	std::printf( "  refraction  %.5f wheel units\n", w.refraction );
	std::printf( "  dispersion  %.4f\n", w.dispersion );
	std::printf( "  meniscus    %.4f\n", w.meniscus );
	std::printf( "  caustic     %.4f\n", w.caustic );
	std::printf( "  rim         %.5f wheel units\n", w.rim );
	std::printf( "  palette     %s\n", PaletteName( w.palette ) );
	std::printf( "  hue         %.4f turns\n", w.hue );
	std::printf( "  hue spread  %.4f turns\n", w.hueSpread );
	std::printf( "  saturation  %.4f\n", w.saturation );
	std::printf( "  lamp        %.4f\n", w.lamp );
	std::printf( "  hotspot     %.4f\n", w.hotspot );
	std::printf( "  temperature %+.4f\n", w.temperature );
	if( w.gate >= kGateOff )
		std::printf( "  gate        off\n" );
	else
		std::printf( "  gate        %.4f wheel units\n", w.gate );
	std::printf( "  gate soft   %.4f\n", w.gateSoft );
}

/// The control mappings and the resolved wheel, as JSON.
///
/// Consumed by `demo/tools/check_cells.mjs`, which compares it against the
/// browser demo's ported copy of the same maths. Until that comparison
/// existed nothing checked the ported half of the demo at all, and on THIS
/// plugin the gap is wide: everything about a cell comes out of `CellAt`, so
/// a mistake in the port is not a slider reading 0.47 instead of 0.5, it is
/// the whole wheel being arranged differently.
int dumpCells()
{
	std::printf( "{\n" );

	//--- the control mappings, sampled across their travel ----------------
	struct Mapping
	{
		const char* name;
		float ( *fn )( float );
	};
	const Mapping mappings[] = {
		{ "size", SizeFromParam },
		{ "merge", MergeFromParam },
		{ "spread", SpreadFromParam },
		{ "speed", SpeedFromParam },
		{ "drift", DriftFromParam },
		{ "spin", SpinFromParam },
		{ "churn", ChurnFromParam },
		{ "grain", GrainFromParam },
		{ "boil", BoilFromParam },
		{ "refraction", RefractionFromParam },
		{ "rim", RimFromParam },
		{ "caustic", CausticFromParam },
		{ "lamp", LampFromParam },
		{ "temperature", TemperatureFromParam },
		{ "gate", GateFromParam },
	};

	std::printf( "  \"mappings\": {\n" );
	for( size_t m = 0; m < sizeof( mappings ) / sizeof( mappings[ 0 ] ); ++m )
	{
		std::printf( "    \"%s\": [", mappings[ m ].name );
		for( int i = 0; i <= 20; ++i )
		{
			const float v = static_cast< float >( i ) / 20.0f;
			std::printf( "%s%.9g", i ? ", " : "", mappings[ m ].fn( v ) );
		}
		std::printf( "]%s\n", m + 1 < sizeof( mappings ) / sizeof( mappings[ 0 ] ) ? "," : "" );
	}
	std::printf( "  },\n" );

	std::printf( "  \"cellsFromParam\": [" );
	for( int i = 0; i <= 20; ++i )
		std::printf( "%s%d", i ? ", " : "", CellsFromParam( static_cast< float >( i ) / 20.0f ) );
	std::printf( "],\n" );

	//--- whole wheels, resolved -------------------------------------------
	struct Case
	{
		const char* name;
		Wheel wheel;
	};

	Wheel base;
	base.cells  = 17;
	base.seed   = 7;
	base.aspect = 16.0f / 9.0f;

	Wheel spun     = base;
	spun.spin      = 0.11f;
	spun.time      = 3.7f;
	spun.scatter   = 0.8f;
	spun.variation = 0.9f;

	Wheel inked      = base;
	inked.palette    = Palette::Ink;
	inked.hue        = 0.3f;
	inked.hueSpread  = 0.6f;
	inked.saturation = 0.75f;
	inked.time       = 1.25f;
	inked.speed      = 0.9f;

	Wheel spectrum     = base;
	spectrum.palette   = Palette::Spectrum;
	spectrum.cells     = 41;
	spectrum.time      = 9.5f;
	spectrum.speed     = 0.4f;
	spectrum.drift     = 0.3f;
	spectrum.seed      = 4242;

	Case cases[] = {
		{ "base", base }, { "spun", spun }, { "inked", inked }, { "spectrum", spectrum }
	};

	//These wheels are built by hand rather than driven by a clock, so they
	//carry a time and a rate and no phase. The demo's port does the same
	//thing at the same point, which is what keeps the two comparable.
	for( Case& c : cases )
		SetFreeRunningPhases( c.wheel );

	std::printf( "  \"wheels\": {\n" );
	for( size_t c = 0; c < sizeof( cases ) / sizeof( cases[ 0 ] ); ++c )
	{
		std::printf( "    \"%s\": [\n", cases[ c ].name );
		for( int i = 0; i < cases[ c ].wheel.cells; ++i )
		{
			const Cell cell = CellAt( i, cases[ c ].wheel );
			std::printf( "      [%.9g, %.9g, %.9g, %.9g, %.9g, %.9g]%s\n",
			             cell.x, cell.y, cell.radius,
			             cell.dye[ 0 ], cell.dye[ 1 ], cell.dye[ 2 ],
			             i + 1 < cases[ c ].wheel.cells ? "," : "" );
		}
		std::printf( "    ]%s\n", c + 1 < sizeof( cases ) / sizeof( cases[ 0 ] ) ? "," : "" );
	}
	std::printf( "  }\n" );

	std::printf( "}\n" );
	return 0;
}

//---------------------------------------------------------------------------
// --field: the GLSL against the C++.
//---------------------------------------------------------------------------
constexpr int kProbeW = 61;///< prime, so no sample lands on a binary fraction
constexpr int kProbeH = 37;///< and cancels a bug out on both sides

/// True when the field's gradient is unstable here -- i.e. the point is
/// sitting on one of the `min`'s deliberate discontinuities. See kGuardStep.
bool nearDiscontinuity( float x, float y, const Cell* cells, int count,
                        float merge, float density, float rim )
{
	const Sample a = FieldAt( x - kGuardStep, y, cells, count, merge, density, rim );
	const Sample b = FieldAt( x + kGuardStep, y, cells, count, merge, density, rim );
	const Sample c = FieldAt( x, y - kGuardStep, cells, count, merge, density, rim );
	const Sample d = FieldAt( x, y + kGuardStep, cells, count, merge, density, rim );

	const float swing = std::max(
		std::max( std::fabs( a.gx - b.gx ), std::fabs( a.gy - b.gy ) ),
		std::max( std::fabs( c.gx - d.gx ), std::fabs( c.gy - d.gy ) ) );

	//A tenth of a unit vector over six ten-thousandths of a wheel is a
	//gradient that is turning far too fast to be geometry.
	return swing > 0.1f;
}

int runFieldCheck()
{
	std::string error;
	const GLuint program = buildProbeProgram( error );
	if( program == 0 )
	{
		std::fprintf( stderr, "the probe shader would not build:\n%s\n", error.c_str() );
		return 1;
	}

	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, kProbeW, kProbeH, 0, GL_RGBA, GL_FLOAT, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glBindTexture( GL_TEXTURE_2D, 0 );

	const GLuint fbo = makeFramebuffer( texture );
	if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
	{
		std::fprintf( stderr, "could not make a float target for the probe\n" );
		return 1;
	}

	//A full-screen triangle, which needs no index buffer and no UVs.
	GLuint vao = 0, vbo = 0;
	glGenVertexArrays( 1, &vao );
	glBindVertexArray( vao );
	const float triangle[] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
	glGenBuffers( 1, &vbo );
	glBindBuffer( GL_ARRAY_BUFFER, vbo );
	glBufferData( GL_ARRAY_BUFFER, sizeof( triangle ), triangle, GL_STATIC_DRAW );
	glEnableVertexAttribArray( 0 );
	glVertexAttribPointer( 0, 2, GL_FLOAT, GL_FALSE, 0, nullptr );

	//The cases. Each turns some controls up and leaves the rest at their
	//nulls, plus two that turn everything up at once -- because a mirrored
	//function can be right on every control taken alone and wrong about the
	//ORDER it applies them in, and only a case with several at once can see
	//that.
	Wheel base;
	base.cells      = 12;
	base.merge      = 0.06f;
	base.density    = 0.8f;
	base.rim        = 0.02f;
	base.churn      = 0.0f;
	base.refraction = 0.0f;
	base.gate       = 1.4f;
	base.gateSoft   = 0.3f;
	base.seed       = 3;

	auto with = [ base ]( auto&& mutate ) {
		Wheel w = base;
		mutate( w );
		return w;
	};

	struct Case
	{
		const char* name;
		Wheel wheel;
	};

	const Case cases[] = {
		{ "null", base },
		{ "one-cell", with( []( Wheel& w ) { w.cells = 1; } ) },
		{ "no-cells", with( []( Wheel& w ) { w.cells = 0; } ) },
		{ "full", with( []( Wheel& w ) { w.cells = kMaxCells; w.size = 0.09f; } ) },
		{ "hard-min", with( []( Wheel& w ) { w.merge = 0.0f; } ) },
		{ "wide-merge", with( []( Wheel& w ) { w.merge = 0.37f; w.size = 0.31f; } ) },
		{ "dense", with( []( Wheel& w ) { w.density = 1.0f; w.size = 0.4f; } ) },
		{ "clear", with( []( Wheel& w ) { w.density = 0.0f; } ) },
		{ "thin-rim", with( []( Wheel& w ) { w.rim = 0.002f; } ) },
		{ "fat-rim", with( []( Wheel& w ) { w.rim = 0.15f; } ) },
		{ "churn", with( []( Wheel& w ) { w.churn = 0.13f; w.grain = 3.1f; w.boilPhase = 2.71f; } ) },
		{ "churn-hard", with( []( Wheel& w ) { w.churn = 0.57f; w.grain = 11.3f; w.boilPhase = -4.13f; } ) },
		{ "refract", with( []( Wheel& w ) { w.refraction = 0.043f; } ) },
		{ "gate-hard", with( []( Wheel& w ) { w.gate = 0.83f; w.gateSoft = 0.0f; } ) },
		{ "gate-soft", with( []( Wheel& w ) { w.gate = 1.13f; w.gateSoft = 1.0f; } ) },
		{ "gate-off", with( []( Wheel& w ) { w.gate = kGateOff; } ) },
		{ "lamp-warm", with( []( Wheel& w ) { w.lamp = 1.7f; w.hotspot = 0.9f; w.temperature = 0.83f; } ) },
		{ "lamp-cool", with( []( Wheel& w ) { w.lamp = 0.4f; w.hotspot = 0.4f; w.temperature = -0.61f; } ) },
		{ "spectrum", with( []( Wheel& w ) {
			 w.palette = Palette::Spectrum; w.cells = 23; w.time = 5.5f; w.speed = 0.7f;
			 w.drift = 0.21f; w.scatter = 0.8f; w.variation = 0.9f; w.seed = 991; } ) },
		{ "everything", with( []( Wheel& w ) {
			 w.cells = 31; w.size = 0.14f; w.variation = 0.7f; w.merge = 0.11f;
			 w.spread = 1.1f; w.scatter = 0.45f; w.seed = 77;
			 w.time = 12.5f; w.speed = 0.9f; w.drift = 0.17f; w.spin = -0.07f;
			 w.churn = 0.21f; w.grain = 5.3f; w.boilPhase = 9.31f;
			 w.density = 0.93f; w.refraction = 0.061f; w.rim = 0.031f;
			 w.lamp = 1.3f; w.hotspot = 0.62f; w.temperature = 0.37f;
			 w.gate = 1.21f; w.gateSoft = 0.55f;
			 w.palette = Palette::Ink; w.hue = 0.41f; w.hueSpread = 0.83f; w.saturation = 0.71f; } ) },
	};

	//Two spans, because the field's interesting territory is near the origin
	//and its edge cases are outside the packed disc.
	const float spans[][ 2 ] = { { 1.9f, 1.2f }, { 3.3f, 2.9f } };

	int checks        = 0;
	int disagreements = 0;
	int skipped       = 0;
	float worst       = 0.0f;
	std::string worstWhere;

	std::vector< float > readback( static_cast< size_t >( kProbeW ) * kProbeH * 4 );

	glUseProgram( program );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glViewport( 0, 0, kProbeW, kProbeH );

	auto setFloat = [ program ]( const char* name, float value ) {
		glUniform1f( glGetUniformLocation( program, name ), value );
	};
	auto setInt = [ program ]( const char* name, int value ) {
		glUniform1i( glGetUniformLocation( program, name ), value );
	};

	auto compare = [ & ]( const char* what, const char* caseName, int slot, int x, int y,
	                      float got, float want ) {
		const float scale = std::max( std::fabs( want ), kRelativeAbove );
		const float diff  = std::fabs( got - want ) / scale;

		++checks;
		if( diff > worst )
		{
			worst = diff;
			char where[ 256 ];
			std::snprintf( where, sizeof( where ), "%s %s slot %d at (%d,%d)",
			               caseName, what, slot, x, y );
			worstWhere = where;
		}
		if( diff > kFieldTolerance )
		{
			if( disagreements < 8 )
				std::printf( "  %-12s %-10s (%2d,%2d)  glsl %-14.7g c++ %-14.7g\n",
				             caseName, what, x, y, got, want );
			++disagreements;
		}
	};

	for( const Case& c : cases )
	{
		std::vector< Cell > cells;
		for( int i = 0; i < c.wheel.cells; ++i )
			cells.push_back( CellAt( i, c.wheel ) );

		std::vector< float > cellPos, cellDye;
		for( const Cell& cell : cells )
		{
			cellPos.push_back( cell.x );
			cellPos.push_back( cell.y );
			cellPos.push_back( cell.radius );
			cellDye.push_back( cell.dye[ 0 ] );
			cellDye.push_back( cell.dye[ 1 ] );
			cellDye.push_back( cell.dye[ 2 ] );
		}

		setInt( "CellCount", static_cast< int >( cells.size() ) );
		if( !cells.empty() )
		{
			glUniform3fv( glGetUniformLocation( program, "CellPos" ),
			              static_cast< GLsizei >( cells.size() ), cellPos.data() );
			glUniform3fv( glGetUniformLocation( program, "CellDye" ),
			              static_cast< GLsizei >( cells.size() ), cellDye.data() );
		}

		setFloat( "Merge", c.wheel.merge );
		setFloat( "Density", c.wheel.density );
		setFloat( "Rim", c.wheel.rim );
		setFloat( "Churn", c.wheel.churn );
		setFloat( "Grain", c.wheel.grain );
		setFloat( "BoilPhase", c.wheel.boilPhase );
		setFloat( "Refraction", c.wheel.refraction );
		setFloat( "Lamp", c.wheel.lamp );
		setFloat( "Hotspot", c.wheel.hotspot );
		setFloat( "Temperature", c.wheel.temperature );
		setFloat( "Gate", c.wheel.gate );
		setFloat( "GateSoft", c.wheel.gateSoft );

		for( const auto& span : spans )
		{
			glUniform2f( glGetUniformLocation( program, "ProbeSpan" ), span[ 0 ], span[ 1 ] );
			glUniform2f( glGetUniformLocation( program, "ProbeSize" ),
			             static_cast< float >( kProbeW ), static_cast< float >( kProbeH ) );

			//Every slot is rendered BEFORE any of it is compared, because
			//two of the mirrored functions take another one's output as their
			//input and the check has to be about the function rather than
			//about the error accumulated on the way in.
			//
			//`RimProfiles` and `BendAt` are the two, and the amplification is
			//real: the caustic and meniscus profiles are Gaussians a fiftieth
			//of a wheel unit wide, so their slope near the surface is of
			//order a hundred, and a difference of 5e-6 in `d` -- which is
			//what a float32 `floor` at a coordinate of seventy-five costs,
			//and what the GPU's freedom to fuse a multiply-add costs on top
			//-- arrives as 4e-4 in the profile. That is not a drifted
			//constant and no tolerance that admitted it would catch one.
			//
			//So `d` and the gradient are compared directly, in slot 0, where
			//they belong; and the profiles are compared against the C++
			//evaluated at the GPU's OWN `d`. Both halves are still checked,
			//and neither is checked against the other's noise.
			float slots[ 5 ][ kProbeH * kProbeW * 4 ];

			for( int slot = 0; slot <= 4; ++slot )
			{
				setInt( "ProbeSlot", slot );

				glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
				glClear( GL_COLOR_BUFFER_BIT );
				glDrawArrays( GL_TRIANGLES, 0, 3 );
				glReadPixels( 0, 0, kProbeW, kProbeH, GL_RGBA, GL_FLOAT, readback.data() );
				std::memcpy( slots[ slot ], readback.data(), readback.size() * sizeof( float ) );
			}

			for( int y = 0; y < kProbeH; ++y )
			{
				for( int x = 0; x < kProbeW; ++x )
				{
					const size_t at = ( static_cast< size_t >( y ) * kProbeW + x ) * 4;

					float px = 0.0f, py = 0.0f;
					ProbePoint( x, y, kProbeW, kProbeH, span[ 0 ], span[ 1 ], px, py );

					float wx = px, wy = py;
					WarpPoint( px, py, c.wheel.churn, c.wheel.grain, c.wheel.boilPhase, wx, wy );

					const Sample s = FieldAt( wx, wy, cells.data(),
					                          static_cast< int >( cells.size() ),
					                          c.wheel.merge, c.wheel.density, c.wheel.rim );

					const bool guarded = nearDiscontinuity( wx, wy, cells.data(),
					                                        static_cast< int >( cells.size() ),
					                                        c.wheel.merge, c.wheel.density,
					                                        c.wheel.rim );

					//--- slot 0: the field itself -------------------------
					const float* f = &slots[ 0 ][ at ];
					compare( "d", c.name, 0, x, y, f[ 0 ], s.d );
					if( guarded )
						skipped += 2;
					else
					{
						compare( "grad.x", c.name, 0, x, y, f[ 1 ], s.gx );
						compare( "grad.y", c.name, 0, x, y, f[ 2 ], s.gy );
					}
					compare( "gate", c.name, 0, x, y, f[ 3 ],
					         GateAt( px, py, c.wheel.gate, c.wheel.gateSoft ) );

					//--- slot 1: the dye stack, and the noise underneath --
					const float* t = &slots[ 1 ][ at ];
					compare( "trans.r", c.name, 1, x, y, t[ 0 ], s.t[ 0 ] );
					compare( "trans.g", c.name, 1, x, y, t[ 1 ], s.t[ 1 ] );
					compare( "trans.b", c.name, 1, x, y, t[ 2 ], s.t[ 2 ] );
					compare( "noise", c.name, 1, x, y, t[ 3 ],
					         Noise2( px * c.wheel.grain, py * c.wheel.grain ) );

					//--- slot 2: the rim, from the GPU's own field --------
					Sample fromGpu;
					fromGpu.d  = f[ 0 ];
					fromGpu.dn = slots[ 4 ][ at + 2 ];
					fromGpu.gx = f[ 1 ];
					fromGpu.gy = f[ 2 ];

					const float* r = &slots[ 2 ][ at ];
					float cst = 0.0f, men = 0.0f;
					RimProfiles( fromGpu.dn, c.wheel.rim, cst, men );
					compare( "caustic", c.name, 2, x, y, r[ 0 ], cst );
					compare( "meniscus", c.name, 2, x, y, r[ 1 ], men );

					if( guarded )
						skipped += 2;
					else
					{
						float bx = 0.0f, by = 0.0f;
						BendAt( fromGpu, c.wheel.refraction, c.wheel.rim, bx, by );
						compare( "bend.x", c.name, 2, x, y, r[ 2 ], bx );
						compare( "bend.y", c.name, 2, x, y, r[ 3 ], by );
					}

					//--- slot 3: the lamp ---------------------------------
					const float* l = &slots[ 3 ][ at ];
					float lr = 0.0f, lg = 0.0f, lb = 0.0f;
					SynthLamp( px, py, c.wheel.lamp, c.wheel.hotspot, c.wheel.temperature, lr, lg, lb );
					compare( "lamp.r", c.name, 3, x, y, l[ 0 ], lr );
					compare( "lamp.g", c.name, 3, x, y, l[ 1 ], lg );
					compare( "lamp.b", c.name, 3, x, y, l[ 2 ], lb );

					//--- slot 4: the churn --------------------------------
					const float* w = &slots[ 4 ][ at ];
					compare( "warp.x", c.name, 4, x, y, w[ 0 ], wx );
					compare( "warp.y", c.name, 4, x, y, w[ 1 ], wy );
					if( guarded )
						++skipped;
					else
						compare( "nearest", c.name, 4, x, y, w[ 2 ], s.dn );
				}
			}
		}
	}

	glDeleteProgram( program );

	std::printf( "field: %d comparisons, %d past %.0e, %d skipped on a discontinuity\n",
	             checks, disagreements, static_cast< double >( kFieldTolerance ), skipped );
	std::printf( "field: largest difference %.3g (%s)\n", worst, worstWhere.c_str() );

	return disagreements == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// Driving the plugin.
//---------------------------------------------------------------------------

/// Drive the plugin's clock. The harness DECLARES its unit rather than
/// leaving the calibration to infer one: an absolute time handed over in a
/// single frame is genuinely ambiguous, and an implicit unit is what let the
/// millisecond bug through elsewhere in the fleet.
void driveClock( FlenserPlugin& plugin, double seconds )
{
	plugin.SetClockScaleForTest( 1.0 );
	plugin.SetTime( seconds );
}

/// Render `frames` frames of the test card through the plugin and return the
/// last one.
///
/// **One row convention, everywhere.** GL treats a texture's first row as the
/// bottom one and reads a framebuffer back the same way; the test card, a
/// PNG, and a raw frame from ffmpeg are all top-down. So the card is uploaded
/// FLIPPED and every readback is flipped again, and the result is that
/// everything outside this function is top-down and nothing has to remember
/// which way round it is.
///
/// The alternative -- upload as-is and flip only where a PNG is written -- is
/// what this did first, and it cost an afternoon: `--null` compared the top
/// of the output against the bottom of the card and reported 255/255 on a
/// plugin that was behaving perfectly, and the two PNGs `--out` and `--card`
/// write came out of the same run the opposite way up. afterglow's `--still`
/// carries a comment about the same trap.
/// `staticCard` builds the test card ONCE and reuses it. Only `--bench` wants
/// that, and it wants it badly: the card is a couple of million pixels of
/// trigonometry per frame on the CPU, and at 1080p it costs more than the
/// plugin does. Timing the harness's own scenery as though it were the render
/// is how a plugin acquires a reputation for being slow -- the first version
/// of this reported 12.6 ms at 1080p and could not tell twelve cells from
/// forty-eight, because neither number was the plugin.
std::vector< unsigned char > renderFrames( FlenserPlugin& plugin, bool overInput,
                                           int width, int height, int frames, double fps,
                                           const std::function< void( int ) >* perFrame = nullptr,
                                           bool staticCard = false )
{
	FFGLViewportStruct viewport { 0, 0, static_cast< FFUInt32 >( width ), static_cast< FFUInt32 >( height ) };
	plugin.InitGL( &viewport );

	std::vector< unsigned char > outputPixels( static_cast< size_t >( width ) * height * 4, 0 );
	const GLuint outputTexture = makeTexture( width, height, outputPixels.data() );
	const GLuint outputFbo     = makeFramebuffer( outputTexture );

	GLuint cardTexture = 0;
	for( int frame = 0; frame < frames; ++frame )
	{
		if( perFrame != nullptr )
			( *perFrame )( frame );

		FFGLTextureStruct input {};
		if( overInput )
		{
			if( cardTexture == 0 || !staticCard )
			{
				const std::vector< unsigned char > card =
					flipRows( buildCard( width, height, frame ), width, height );
				if( cardTexture != 0 )
					glDeleteTextures( 1, &cardTexture );
				cardTexture = makeTexture( width, height, card.data() );
			}
			input.Width          = width;
			input.Height         = height;
			input.HardwareWidth  = width;
			input.HardwareHeight = height;
			input.Handle         = cardTexture;
		}

		FFGLTextureStruct* inputs[ 1 ] = { &input };
		ProcessOpenGLStruct process {};
		process.numInputTextures = overInput ? 1 : 0;
		process.inputTextures    = inputs;
		process.HostFBO          = outputFbo;

		driveClock( plugin, static_cast< double >( frame ) / fps );

		glBindFramebuffer( GL_FRAMEBUFFER, outputFbo );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		plugin.ProcessOpenGL( &process );
	}

	std::vector< unsigned char > result = readBackRaw( outputFbo, width, height );

	if( cardTexture != 0 )
		glDeleteTextures( 1, &cardTexture );
	glDeleteFramebuffers( 1, &outputFbo );
	glDeleteTextures( 1, &outputTexture );
	plugin.DeInitGL();

	return flipRows( result, width, height );
}

//---------------------------------------------------------------------------
/// --null: a clear wheel does not touch the clip.
///
/// With no dye, no refraction, no meniscus and no caustic, the effect build
/// is a piece of clear glass in front of the lamp -- and with the lamp at
/// unity, no hot spot, a neutral temperature and the gate off, the picture
/// has to come out exactly as it went in.
///
/// This is the check that stands between the plugin and a whole class of
/// defect that nothing else here would notice: a half-texel error in the clip
/// fetch, a MaxUV that was applied twice or not at all, an intermediate
/// buffer at the wrong precision, a composite that lifts black. All of them
/// look like "it's a bit soft" or "it's a bit washed out" rather than like a
/// bug, and all of them are invisible against a picture that the oil is
/// already changing.
//---------------------------------------------------------------------------
int runNullCheck( int width, int height )
{
	FlenserPlugin plugin( true );

	struct Setting
	{
		const char* name;
		float value;
	};
	//Everything the oil does to the light, off; everything the projector does
	//to it, neutral.
	const Setting settings[] = {
		{ "Density", 0.0f },
		{ "Refraction", 0.0f },
		{ "Meniscus", 0.0f },
		{ "Caustic", 0.0f },
		{ "Lamp", 0.5f },       //unity after mapping
		{ "Hotspot", 0.0f },
		{ "Temperature", 0.5f },//bipolar null
		{ "Gate", 1.0f },       //off
		{ "Mix", 1.0f },
		{ "Simmer", 0.0f },
	};

	for( const Setting& s : settings )
	{
		std::string error;
		if( !applySetting( plugin, std::string( s.name ) + "=" + std::to_string( s.value ), error ) )
		{
			std::fprintf( stderr, "null: %s\n", error.c_str() );
			return 1;
		}
	}

	const std::vector< unsigned char > output = renderFrames( plugin, true, width, height, 3, 60.0 );
	const std::vector< unsigned char > card   = buildCard( width, height, 2 );

	size_t differ = 0;
	int worst     = 0;
	size_t worstAt = 0;
	for( size_t i = 0; i < card.size(); ++i )
	{
		const int d = std::abs( static_cast< int >( output[ i ] ) - static_cast< int >( card[ i ] ) );
		if( d != 0 )
		{
			++differ;
			if( d > worst )
			{
				worst   = d;
				worstAt = i;
			}
		}
	}

	std::printf( "null: %zu of %zu bytes differ from the input", differ, card.size() );
	if( differ != 0 )
		std::printf( ", worst %d/255 at byte %zu (pixel %zu,%zu)",
		             worst, worstAt,
		             ( worstAt / 4 ) % static_cast< size_t >( width ),
		             ( worstAt / 4 ) / static_cast< size_t >( width ) );
	std::printf( "\n" );

	return differ == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --bench.
//---------------------------------------------------------------------------
double benchAt( FlenserPlugin& plugin, bool overInput, int width, int height, int frames, double fps )
{
	//One warm frame first: the first call compiles nothing but does allocate
	//every buffer, and timing an allocation as though it were a render is how
	//a plugin acquires a reputation for a slow first frame it does not have.
	renderFrames( plugin, overInput, width, height, 1, fps, nullptr, true );

	const auto start = std::chrono::steady_clock::now();
	renderFrames( plugin, overInput, width, height, frames, fps, nullptr, true );
	glFinish();
	const auto end = std::chrono::steady_clock::now();

	return std::chrono::duration< double, std::milli >( end - start ).count() / frames;
}

//---------------------------------------------------------------------------
// --continuity: moving a rate control does not move the picture.
//
// Speed and Spin set a RATE. Changing a rate has to change what happens next
// and nothing else -- if the phase is `time * rate`, changing the rate
// rescales the entire history, and the wheel jumps to wherever that lands.
// Five seconds in it is a visible lurch; an hour into a show a small nudge is
// worth hundreds of turns and the arrangement is simply gone. Reported from a
// live rig as "unusable in performance" (#1), and the same defect orrery had.
//
// The assertion is exact rather than a threshold, which is what makes it
// worth having: on the frame the control moves, the anchor carries the phase
// forward and the new rate has had zero seconds to act, so that frame must be
// **bit-identical** to the frame that would have been drawn had nothing been
// touched. Then the frame after it must differ, or the control is dead.
//
// Both halves are needed. The first alone passes if the control does nothing
// at all; the second alone passes on the very code this replaces.
//---------------------------------------------------------------------------
int runContinuityCheck( int width, int height )
{
	constexpr double kFps  = 60.0;
	constexpr int kLead    = 300;///< 5 s at 60 fps, so a rescale is unmissable

	// Motion the change could disturb, and nothing else moving that might
	// mask a jump: no boil, no churn, no feedback.
	struct Setting
	{
		const char* name;
		float value;
	};
	const Setting bed[] = {
		{ "Drift", 0.8f }, { "Boil", 0.0f }, { "Churn", 0.0f }, { "Simmer", 0.0f },
	};

	struct Case
	{
		const char* control;
		float from;
		float to;
	};
	//Both are rates, and Spin is bipolar -- 0.5 is its null, so this is a
	//change of direction as well as of size.
	const Case cases[] = {
		{ "Speed", 0.20f, 0.62f },
		{ "Spin", 0.60f, 0.42f },
	};

	// One run of `frames` frames, optionally moving `control` to `to` at
	// frame `changeAt`. A fresh plugin every time: the anchors are state, and
	// reusing one would carry the previous run's into this one.
	auto run = [ & ]( const Case& c, int frames, int changeAt ) {
		FlenserPlugin plugin( false );
		std::string error;
		for( const Setting& b : bed )
			applySetting( plugin, std::string( b.name ) + "=" + std::to_string( b.value ), error );
		applySetting( plugin, std::string( c.control ) + "=" + std::to_string( c.from ), error );

		const std::function< void( int ) > perFrame = [ & ]( int frame ) {
			if( changeAt >= 0 && frame == changeAt )
			{
				std::string e;
				applySetting( plugin, std::string( c.control ) + "=" + std::to_string( c.to ), e );
			}
		};
		return renderFrames( plugin, false, width, height, frames, kFps, &perFrame );
	};

	int failures = 0;
	for( const Case& c : cases )
	{
		const std::vector< unsigned char > undisturbed = run( c, kLead + 1, -1 );
		const std::vector< unsigned char > atTheChange = run( c, kLead + 1, kLead );
		const std::vector< unsigned char > afterwards  = run( c, kLead + 2, kLead );
		const std::vector< unsigned char > wouldHaveBeen = run( c, kLead + 2, -1 );

		size_t jumped = 0;
		for( size_t i = 0; i < undisturbed.size() && i < atTheChange.size(); ++i )
			if( undisturbed[ i ] != atTheChange[ i ] )
				++jumped;

		size_t moved = 0;
		for( size_t i = 0; i < afterwards.size() && i < wouldHaveBeen.size(); ++i )
			if( afterwards[ i ] != wouldHaveBeen[ i ] )
				++moved;

		if( jumped != 0 )
		{
			std::printf( "continuity: %s MOVED THE PICTURE -- %zu of %zu bytes differ on the frame "
			             "it changed\n",
			             c.control, jumped, undisturbed.size() );
			++failures;
		}
		else if( moved == 0 )
		{
			std::printf( "continuity: %s changed nothing at all on the frame after -- dead, not "
			             "continuous\n",
			             c.control );
			++failures;
		}
		else
		{
			std::printf( "continuity: %s holds the picture on the frame it moves, and %zu of %zu "
			             "bytes differ one frame later\n",
			             c.control, moved, afterwards.size() );
		}
	}

	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --matte: the cutout mode does what it says.
//
// Matte is Over composited against nothing, so the clip plays no part in it.
// Two things follow, and both are worth pinning down because both are the
// reason the mode was added (#2):
//
//   * the generator and the effect render it IDENTICALLY -- it is the only
//     mode of which that is true, and it is what lets one set of numbers
//     document both plugins;
//   * where the oil covers nothing the pixel is transparent AND black, since
//     the output is premultiplied. A host that ignores alpha still gets the
//     black field somebody asked for.
//
// Without the second check the mode could quietly become "opaque black
// everywhere", which keys as nothing at all and looks fine in a screenshot.
//
// **One quantisation step of slack on that second check, and only one.** The
// lamp is brighter than white where a caustic lands, so `col` exceeds 1 and a
// pixel whose coverage rounds to alpha 0 in eight bits can still round its
// premultiplied colour up to 1/255. That is the arithmetic being right at the
// edge of the format, not a premultiplication error -- which would leave the
// dye's full colour standing at alpha 0, hundreds of steps away, and is what
// this still catches.
//---------------------------------------------------------------------------
int runMatteCheck( int width, int height )
{
	const int mode = static_cast< int >( LampMode::Matte );

	auto render = [ & ]( bool overInput ) {
		FlenserPlugin plugin( overInput );
		std::string error;
		applySetting( plugin, "Mode=" + std::to_string( mode ), error );
		return renderFrames( plugin, overInput, width, height, 2, 60.0 );
	};

	const std::vector< unsigned char > generator = render( false );
	const std::vector< unsigned char > effect    = render( true );

	int failures = 0;

	size_t differ = 0;
	for( size_t i = 0; i < generator.size() && i < effect.size(); ++i )
		if( generator[ i ] != effect[ i ] )
			++differ;

	if( differ != 0 )
	{
		std::printf( "matte: the generator and the effect disagree -- %zu of %zu bytes\n",
		             differ, generator.size() );
		++failures;
	}
	else
	{
		std::printf( "matte: the generator and the effect are identical, %zu bytes\n",
		             generator.size() );
	}

	// Premultiplied: every fully transparent pixel must be black, and some of
	// the frame must be transparent or the mode is not cutting anything out.
	constexpr unsigned char kQuantisationSlack = 1;

	size_t clear   = 0;
	size_t lit     = 0;
	size_t dirty   = 0;
	unsigned char worst = 0;
	for( size_t i = 0; i + 3 < generator.size(); i += 4 )
	{
		if( generator[ i + 3 ] == 0 )
		{
			++clear;
			const unsigned char brightest =
				std::max( { generator[ i ], generator[ i + 1 ], generator[ i + 2 ] } );
			worst = std::max( worst, brightest );
			if( brightest > kQuantisationSlack )
				++dirty;
		}
		else if( generator[ i + 3 ] == 255 )
		{
			++lit;
		}
	}

	if( dirty != 0 )
	{
		std::printf( "matte: %zu transparent pixels carry colour up to %u/255 -- not "
		             "premultiplied\n",
		             dirty, static_cast< unsigned >( worst ) );
		++failures;
	}
	else if( clear == 0 )
	{
		std::printf( "matte: nothing is transparent -- the mode cut nothing out\n" );
		++failures;
	}
	else
	{
		std::printf( "matte: %zu of %zu pixels fully transparent and black to within %u/255, "
		             "%zu fully opaque\n",
		             clear, generator.size() / 4, static_cast< unsigned >( worst ), lit );
	}

	return failures == 0 ? 0 : 1;
}

int runBench( bool overInput, int frames, double fps )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = { { "720p", 1280, 720 }, { "1080p", 1920, 1080 }, { "4K", 3840, 2160 } };

	std::printf( "bench: %d frames each, %s build\n", frames, overInput ? "effect" : "source" );
	std::printf( "       (a warm frame is rendered and discarded first; the buffers are\n" );
	std::printf( "        allocated on it, and timing that as a render is how a plugin\n" );
	std::printf( "        acquires a reputation for a slow first frame it does not have)\n" );

	for( const Size& size : sizes )
	{
		FlenserPlugin plain( overInput );
		const double defaults = benchAt( plain, overInput, size.width, size.height, frames, fps );

		FlenserPlugin heavy( overInput );
		std::string error;
		applySetting( heavy, "Cells=1.0", error );
		applySetting( heavy, "Simmer=0.8", error );
		const double loaded = benchAt( heavy, overInput, size.width, size.height, frames, fps );

		std::printf( "  %-6s  defaults %6.2f ms   48 cells + simmer %6.2f ms\n",
		             size.name, defaults, loaded );
	}

	return 0;
}

//---------------------------------------------------------------------------
// --presets: every factory preset survives every host behaviour.
//
// FFGL's host owns parameter state and is free to push it back down at any
// time, and nothing in the specification obliges it to act on the value
// events a plugin raises when it changes a parameter itself. So there are
// three hosts to survive, and the plugin cannot tell which one it is talking
// to:
//
//   - one that honours the events and hands the new values straight back;
//   - one that ignores them and carries on restating the values it still
//     believes in, which are the ones from before the preset;
//   - one that honours them but keeps its parameters shorter than a float,
//     so what comes back is near the preset rather than equal to it.
//
// All three arrive as SetFloatParameter calls carrying a changed value, which
// is why "the value changed, so the operator must have taken over" is the
// wrong test. Resolume is the second kind, and against the pattern this
// plugin was copied from it fails in exactly that column -- reported as
// vertigo issue #2.
//
// No GL here: this is the parameter plumbing, not the picture.
//---------------------------------------------------------------------------
int runPresetTest()
{
	using namespace flenser::presets;

	int coveredCount            = 0;
	const unsigned int* covered = FlenserPlugin::PresetParamIDsForTest( coveredCount );

	enum class Host
	{
		Honours,
		Ignores,
		Quantises
	};
	struct HostCase
	{
		Host kind;
		const char* name;
	};
	const HostCase hosts[] = {
		{ Host::Honours, "honours value events" },
		{ Host::Ignores, "ignores value events" },
		{ Host::Quantises, "honours, 1/1000 steps" },
	};

	int failures = 0;

	//Both bundles, because they declare the same parameters from the same
	//constructor and a preset that worked in one and not the other would mean
	//that had stopped being true.
	for( int variant = 0; variant < 2; ++variant )
	{
		for( const HostCase& host : hosts )
		{
			for( int preset = 1; preset <= kCount; ++preset )
			{
				FlenserPlugin plugin( variant == 1 );

				const unsigned int presetIndex = paramIndexByName( plugin, "Preset" );
				if( presetIndex == 0xffffffffu )
				{
					std::fprintf( stderr, "presets: no parameter is called \"Preset\"\n" );
					return 1;
				}

				// What the host thinks the sliders say before the operator
				// reaches for the dropdown.
				std::vector< float > hostOwn;
				for( int j = 0; j < coveredCount; ++j )
					hostOwn.push_back( plugin.GetFloatParameter( covered[ j ] ) );

				// The operator picks a preset.
				plugin.SetFloatParameter( presetIndex, static_cast< float >( preset ) );

				// And now the host says its piece.
				for( int j = 0; j < coveredCount; ++j )
				{
					float back = 0.0f;
					switch( host.kind )
					{
					case Host::Honours:
						back = plugin.GetFloatParameter( covered[ j ] );
						break;
					case Host::Ignores:
						back = hostOwn[ size_t( j ) ];
						break;
					case Host::Quantises:
						back = std::round( plugin.GetFloatParameter( covered[ j ] ) * 1000.0f ) / 1000.0f;
						break;
					}
					plugin.SetFloatParameter( covered[ j ], back );
				}

				const int still = int( std::lround( plugin.GetFloatParameter( presetIndex ) ) );
				bool ok         = still == preset;

				// Still selected is not enough -- it has to be what renders.
				for( int j = 0; j < coveredCount; ++j )
				{
					const float want = kPresets[ preset - 1 ].v[ j ];
					const float got  = plugin.GetFloatParameter( covered[ j ] );
					ok               = ok && std::fabs( got - want ) <= 1e-4f;
				}

				if( !ok )
				{
					std::printf( "presets %-7s %-22s %-20s FAILED (shows %d)\n",
					             variant == 1 ? "effect" : "source",
					             host.name, kPresets[ preset - 1 ].name, still );
					++failures;
					continue;
				}

				// An operator turning a covered knob must still drop to Custom
				// -- a preset that cannot be left is no better than one that
				// will not stick. Move it somewhere neither the preset nor the
				// host named.
				const float moved = kPresets[ preset - 1 ].v[ 0 ] > 0.5f ? 0.123f : 0.877f;
				plugin.SetFloatParameter( covered[ 0 ], moved );
				const int after = int( std::lround( plugin.GetFloatParameter( presetIndex ) ) );
				if( after != 0 )
				{
					std::printf( "presets %-7s %-22s %-20s FAILED (an edit left it on %d)\n",
					             variant == 1 ? "effect" : "source",
					             host.name, kPresets[ preset - 1 ].name, after );
					++failures;
				}
			}
		}
	}

	std::printf( "presets: %d cases, %s\n",
	             2 * 3 * kCount, failures == 0 ? "all ok" : "FAILURES" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --script: a keyframe track per parameter.
//---------------------------------------------------------------------------
struct Track
{
	std::vector< std::pair< int, float > > keys;

	float at( int frame ) const
	{
		if( keys.empty() )
			return 0.0f;
		if( frame <= keys.front().first )
			return keys.front().second;
		if( frame >= keys.back().first )
			return keys.back().second;

		for( size_t i = 1; i < keys.size(); ++i )
		{
			if( frame <= keys[ i ].first )
			{
				const auto& a = keys[ i - 1 ];
				const auto& b = keys[ i ];
				const float t = static_cast< float >( frame - a.first )
				                / static_cast< float >( std::max( 1, b.first - a.first ) );
				return a.second + ( b.second - a.second ) * t;
			}
		}
		return keys.back().second;
	}
};

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot read " + path;
		return tracks;
	}

	std::string line;
	while( std::getline( file, line ) )
	{
		if( line.empty() || line[ 0 ] == '#' )
			continue;

		std::istringstream in( line );
		int frame = 0;
		if( !( in >> frame ) )
			continue;

		//The parameter name may have spaces in it, so the VALUE is taken from
		//the end of the line and the name is whatever is between.
		std::string rest;
		std::getline( in, rest );
		const size_t lastSpace = rest.find_last_of( " \t" );
		if( lastSpace == std::string::npos )
			continue;

		const float value = std::strtof( rest.substr( lastSpace + 1 ).c_str(), nullptr );
		std::string name  = rest.substr( 0, lastSpace );
		const size_t from = name.find_first_not_of( " \t" );
		const size_t to   = name.find_last_not_of( " \t" );
		if( from == std::string::npos )
			continue;
		name = name.substr( from, to - from + 1 );

		tracks[ name ].keys.push_back( { frame, value } );
	}

	for( auto& track : tracks )
		std::sort( track.second.keys.begin(), track.second.keys.end() );

	return tracks;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"fltest -- render Flenser offline and check what the oil is doing\n"
		"\n"
		"  --out PATH        render a frame to a PNG\n"
		"  --card PATH       write the test card on its own\n"
		"  --source          drive the generator (default: the effect)\n"
		"  --list            every parameter, its default, and what it resolves to\n"
		"  --cells           the wheel and the control mappings, as JSON\n"
		"  --field           the GLSL oil library against the C++ one\n"
		"  --null            a clear wheel does not touch the clip\n"
		"  --presets         every factory preset survives every host behaviour\n"
		"  --bench           the render cost\n"
		"  --pipe            raw RGBA frames in on stdin, out on stdout\n"
		"\n"
		"  --set \"Name=v\"    set a parameter by display name (repeatable)\n"
		"  --preset NAME     apply a factory preset before any --set\n"
		"  --width N  --height N  --frames N  --fps N\n" );
}
} // namespace

//---------------------------------------------------------------------------
int main( int argc, char** argv )
{
	std::string outPath;
	std::string cardPath;
	std::string scriptPath;
	std::string presetName;
	std::vector< std::string > settings;

	bool wantList    = false;
	bool wantCells   = false;
	bool wantField   = false;
	bool wantNull    = false;
	bool wantContinuity = false;
	bool wantMatte   = false;
	bool wantPresets = false;
	bool wantBench   = false;
	bool wantPipe    = false;
	bool overInput   = true;

	int width   = 640;
	int height  = 360;
	int frames  = 30;
	double fps  = 60.0;

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		auto next                  = [ & ]() { return i + 1 < argc ? std::string( argv[ ++i ] ) : std::string(); };

		if( argument == "--out" )
			outPath = next();
		else if( argument == "--card" )
			cardPath = next();
		else if( argument == "--script" )
			scriptPath = next();
		else if( argument == "--preset" )
			presetName = next();
		else if( argument == "--set" )
			settings.push_back( next() );
		else if( argument == "--width" )
			width = std::atoi( next().c_str() );
		else if( argument == "--height" )
			height = std::atoi( next().c_str() );
		else if( argument == "--frames" )
			frames = std::atoi( next().c_str() );
		else if( argument == "--fps" )
			fps = std::atof( next().c_str() );
		else if( argument == "--source" )
			overInput = false;
		else if( argument == "--effect" )
			overInput = true;
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--cells" )
			wantCells = true;
		else if( argument == "--field" )
			wantField = true;
		else if( argument == "--null" )
			wantNull = true;
		else if( argument == "--continuity" )
			wantContinuity = true;
		else if( argument == "--matte" )
			wantMatte = true;
		else if( argument == "--presets" )
			wantPresets = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else
		{
			usage();
			return argument == "--help" || argument == "-h" ? 0 : 2;
		}
	}

	if( argc == 1 )
	{
		usage();
		return 0;
	}

	//--cells and --presets need no GL at all, and saying so keeps them usable
	//on a machine or a CI runner that has none.
	if( wantCells && !wantField && !wantNull && !wantBench && outPath.empty() && cardPath.empty() )
		return dumpCells();

	if( wantPresets && !wantField && !wantNull && !wantBench && outPath.empty() && cardPath.empty() )
		return runPresetTest();

	if( !cardPath.empty() && !wantField && !wantNull && outPath.empty() )
	{
		const std::vector< unsigned char > card = buildCard( width, height, frames - 1 );
		if( !writePng( cardPath, width, height, card ) )
		{
			std::fprintf( stderr, "could not write %s\n", cardPath.c_str() );
			return 1;
		}
		std::printf( "wrote %s\n", cardPath.c_str() );
		return 0;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "no GL context; this harness needs one\n" );
		return 1;
	}

	int status = 0;

	if( wantField )
		status |= runFieldCheck();

	if( wantNull )
		status |= runNullCheck( width, height );

	if( wantContinuity )
		status |= runContinuityCheck( width, height );

	if( wantMatte )
		status |= runMatteCheck( width, height );

	if( wantPresets )
		status |= runPresetTest();

	if( wantCells )
		status |= dumpCells();

	if( wantBench )
		status |= runBench( overInput, frames, fps );

	if( wantList || !outPath.empty() || wantPipe )
	{
		FlenserPlugin plugin( overInput );

		if( !presetName.empty() )
		{
			const int index = presetIndexByName( plugin, presetName );
			if( index < 0 )
			{
				std::fprintf( stderr, "no preset called \"%s\"\n", presetName.c_str() );
				CGLDestroyContext( context );
				return 1;
			}
			plugin.SetFloatParameter( paramIndexByName( plugin, "Preset" ), static_cast< float >( index ) );
		}

		for( const std::string& setting : settings )
		{
			std::string error;
			if( !applySetting( plugin, setting, error ) )
			{
				std::fprintf( stderr, "%s\n", error.c_str() );
				CGLDestroyContext( context );
				return 2;
			}
		}

		if( wantList )
		{
			std::printf( "%-4s %-24s %s\n", "id", "name", "value" );
			for( const NamedParameter& p : listParameters( plugin ) )
				std::printf( "%-4u %-24s %.4f\n", p.index, p.name.c_str(), p.value );
			printResolved( plugin );
		}

		std::map< std::string, Track > tracks;
		if( !scriptPath.empty() )
		{
			std::string error;
			tracks = loadScript( scriptPath, error );
			if( !error.empty() )
			{
				std::fprintf( stderr, "%s\n", error.c_str() );
				CGLDestroyContext( context );
				return 1;
			}
		}

		std::vector< NamedParameter > named = listParameters( plugin );
		auto applyTracks                    = [ & ]( int frame ) {
            for( const auto& track : tracks )
                for( const NamedParameter& p : named )
                    if( p.name == track.first )
                        plugin.SetFloatParameter( p.index, track.second.at( frame ) );
		};
		const std::function< void( int ) > perFrame = applyTracks;

		if( wantPipe )
		{
			//Raw RGBA in, raw RGBA out, one frame at a time. The fleet's
			//format, so one build.py can film any plugin in the set.
			FFGLViewportStruct viewport { 0, 0, static_cast< FFUInt32 >( width ),
				                          static_cast< FFUInt32 >( height ) };
			plugin.InitGL( &viewport );

			std::vector< unsigned char > blank( static_cast< size_t >( width ) * height * 4, 0 );
			const GLuint outputTexture = makeTexture( width, height, blank.data() );
			const GLuint outputFbo     = makeFramebuffer( outputTexture );

			std::vector< unsigned char > incoming( static_cast< size_t >( width ) * height * 4 );
			GLuint inputTexture = 0;

			for( int frame = 0;; ++frame )
			{
				size_t got = 0;
				while( got < incoming.size() )
				{
					const ssize_t n = read( STDIN_FILENO, incoming.data() + got, incoming.size() - got );
					if( n <= 0 )
						break;
					got += static_cast< size_t >( n );
				}
				if( got < incoming.size() )
					break;

				applyTracks( frame );

				//Top-down in, top-down out: the same convention renderFrames
				//uses, so a frame from ffmpeg and the test card go through
				//identically.
				const std::vector< unsigned char > uploaded =
					flipRows( incoming, width, height );
				if( inputTexture != 0 )
					glDeleteTextures( 1, &inputTexture );
				inputTexture = makeTexture( width, height, uploaded.data() );

				FFGLTextureStruct input {};
				input.Width          = width;
				input.Height         = height;
				input.HardwareWidth  = width;
				input.HardwareHeight = height;
				input.Handle         = inputTexture;

				FFGLTextureStruct* inputs[ 1 ] = { &input };
				ProcessOpenGLStruct process {};
				process.numInputTextures = overInput ? 1 : 0;
				process.inputTextures    = inputs;
				process.HostFBO          = outputFbo;

				driveClock( plugin, static_cast< double >( frame ) / fps );

				glBindFramebuffer( GL_FRAMEBUFFER, outputFbo );
				glViewport( 0, 0, width, height );
				glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
				glClear( GL_COLOR_BUFFER_BIT );
				plugin.ProcessOpenGL( &process );

				const std::vector< unsigned char > out =
					flipRows( readBackRaw( outputFbo, width, height ), width, height );
				fwrite( out.data(), 1, out.size(), stdout );
			}

			fflush( stdout );
			if( inputTexture != 0 )
				glDeleteTextures( 1, &inputTexture );
			glDeleteFramebuffers( 1, &outputFbo );
			glDeleteTextures( 1, &outputTexture );
			plugin.DeInitGL();
		}
		else if( !outPath.empty() )
		{
			const std::vector< unsigned char > image =
				renderFrames( plugin, overInput, width, height, frames, fps,
				              tracks.empty() ? nullptr : &perFrame );

			if( !writePng( outPath, width, height, image ) )
			{
				std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
				status |= 1;
			}
			else
			{
				std::printf( "wrote %s (%dx%d, frame %d)\n", outPath.c_str(), width, height, frames - 1 );
			}

			if( !cardPath.empty() )
			{
				const std::vector< unsigned char > card = buildCard( width, height, frames - 1 );
				if( !writePng( cardPath, width, height, card ) )
					status |= 1;
				else
					std::printf( "wrote %s\n", cardPath.c_str() );
			}
		}
	}

	CGLDestroyContext( context );
	return status;
}
