#pragma once

#include <cstdint>

/**
	An exact integer hash, and the value noise built on it.

	Everything arbitrary in Flenser comes from here: which way a cell drifts,
	how big it is, where its dye sits on the wheel, and the two octaves of
	noise that make the oil churn.

	It is an integer hash and not the usual `fract( sin( x ) * 43758.5453 )`
	for the reason that decides it in every mirrored plugin in the fleet: the
	usual one is transcendental, and GLSL gives `sin` **no accuracy
	requirement at all**. Its result differs between GPUs, between drivers,
	and between a GPU and a CPU. Flenser evaluates the same field twice --
	once in GLSL for the FFGL build and once in C++ for the OpenFX build --
	and `tools/fltest --field` compares them to five decimal places. With a
	sine in the noise those two builds could not be made to agree even in
	principle, and the comparison would have to be deleted rather than fixed.

	The second reason is quieter and applies to the FFGL build on its own: a
	composition built on the show laptop and opened on the rack machine has to
	put the cells in the same places. `lowbias32` is exact everywhere.

	`lowbias32` is Chris Wellons' 32-bit integer bijection, chosen for having
	about the lowest avalanche bias of any two-round xorshift-multiply. The
	GLSL transcription lives in `kOilLibrary` in Shaders.cpp; every line of it
	is marked `//= mirrored` in both files.
*/
namespace flenser
{
inline uint32_t Hash32( uint32_t x )
{
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

/// Mix two values into one hash. Used as `Hash2( cellIndex, seed )` so that
/// nudging Seed reshuffles every cell rather than rotating the set.
inline uint32_t Hash2( uint32_t a, uint32_t b )
{
	return Hash32( a ^ Hash32( b + 0x9e3779b9U ) );
}

inline uint32_t Hash3( uint32_t a, uint32_t b, uint32_t c )
{
	return Hash32( a ^ Hash32( b ^ Hash32( c + 0x9e3779b9U ) ) );
}

/// A hash to 0..1.
///
/// Takes the **top 24 bits**. That is the widest slice that converts to a
/// float32 without rounding -- a float32 has a 24-bit significand -- so the
/// conversion is exact and two machines cannot disagree in the last bit.
/// Taking the low 24 instead would be a rounding on every value, which is
/// exactly the kind of difference `--field` reports and nobody can explain.
inline float Unit( uint32_t h )
{
	return static_cast< float >( h >> 8 ) * ( 1.0f / 16777216.0f );
}

/// A hash to -1..1.
inline float Signed( uint32_t h )
{
	return Unit( h ) * 2.0f - 1.0f;
}

} // namespace flenser
