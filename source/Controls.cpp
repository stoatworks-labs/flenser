#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace flenser
{
namespace
{
inline float clamp01( float value )
{
	return std::min( std::max( value, 0.0f ), 1.0f );
}

inline float lerp( float from, float to, float t )
{
	return from + ( to - from ) * clamp01( t );
}

/// Geometric interpolation. Equal slider movements are equal *ratios*, which
/// is the right behaviour for any quantity where the question is "how many
/// times more" rather than "how much more".
inline float geometric( float from, float to, float t )
{
	return from * std::pow( to / from, clamp01( t ) );
}

/// Geometric, but genuinely off at the bottom of the travel.
///
/// A geometric mapping cannot reach zero, so every control whose null matters
/// gets its floor cut away at exactly 0. The alternative -- a floor small
/// enough "not to matter" -- is how a plugin acquires a refraction of a
/// sixteenth of a pixel that nobody can switch off and everybody works
/// around.
inline float geometricFromZero( float value, float from, float to )
{
	if( value <= 0.0f )
		return 0.0f;

	return geometric( from, to, value );
}

/// A bipolar control: 0.5 is the null, and each half maps to one direction.
///
/// Written as a signed magnitude rather than as `lerp( -limit, limit, v )`
/// because this way the null is exactly 0 at exactly 0.5 rather than within a
/// float's worth of it. A wheel that turns imperceptibly with Spin at its
/// default is the kind of defect nobody reports and everybody works around.
inline float bipolar( float value, float limit )
{
	const float signed_ = clamp01( value ) * 2.0f - 1.0f;
	return signed_ * limit;
}
} // namespace

int CellsFromParam( float value )
{
	return static_cast< int >( std::lround( geometric( 1.0f, 48.0f, value ) ) );
}

float SizeFromParam( float value )
{
	return geometric( 0.03f, 0.9f, value );
}

float VariationFromParam( float value )
{
	return clamp01( value );
}

float MergeFromParam( float value )
{
	return geometricFromZero( value, 0.004f, 0.5f );
}

float SpreadFromParam( float value )
{
	return lerp( 0.0f, 1.4f, value );
}

float ScatterFromParam( float value )
{
	return clamp01( value );
}

int SeedFromParam( float value )
{
	return static_cast< int >( std::lround( lerp( 1.0f, 9999.0f, value ) ) );
}

float SpeedFromParam( float value )
{
	return lerp( 0.0f, 1.5f, value );
}

float DriftFromParam( float value )
{
	return geometricFromZero( value, 0.002f, 0.5f );
}

float SpinFromParam( float value )
{
	return bipolar( value, 0.25f );
}

float ChurnFromParam( float value )
{
	return geometricFromZero( value, 0.004f, 0.6f );
}

float GrainFromParam( float value )
{
	return geometric( 0.5f, 12.0f, value );
}

float BoilFromParam( float value )
{
	return lerp( 0.0f, 2.0f, value );
}

float DensityFromParam( float value )
{
	return clamp01( value );
}

float RefractionFromParam( float value )
{
	return geometricFromZero( value, 0.0004f, 0.12f );
}

float DispersionFromParam( float value )
{
	return clamp01( value );
}

float MeniscusFromParam( float value )
{
	return clamp01( value );
}

float CausticFromParam( float value )
{
	return lerp( 0.0f, 2.0f, value );
}

float RimFromParam( float value )
{
	return geometric( 0.002f, 0.15f, value );
}

float HueFromParam( float value )
{
	return clamp01( value );
}

float HueSpreadFromParam( float value )
{
	return clamp01( value );
}

float SaturationFromParam( float value )
{
	return clamp01( value );
}

float LampFromParam( float value )
{
	return lerp( 0.0f, 2.0f, value );
}

float HotspotFromParam( float value )
{
	return clamp01( value );
}

float TemperatureFromParam( float value )
{
	return bipolar( value, 1.0f );
}

float GateFromParam( float value )
{
	//Off at the top of the travel. See the note in Controls.h: a gate that
	//only ever got large would still crop the corners of a wide canvas at its
	//maximum setting.
	if( value >= 1.0f )
		return kGateOff;

	return geometric( 0.2f, 2.6f, value );
}

float GateSoftFromParam( float value )
{
	return clamp01( value );
}

float SimmerFromParam( float value )
{
	return clamp01( value );
}

float SmearFromParam( float value )
{
	return lerp( 0.0f, 0.04f, value );
}

int PaletteFromParam( float optionValue )
{
	//The option's stored VALUE, not its position in the list -- the list is
	//declared alphabetically and Aniline does not sort first by accident in
	//every locale. See the declaration in Flenser.cpp.
	const int v = static_cast< int >( std::lround( optionValue ) );
	return ( v < 0 || v > 5 ) ? 0 : v;
}

int ModeFromParam( float optionValue )
{
	const int v = static_cast< int >( std::lround( optionValue ) );
	return ( v < 0 || v > 2 ) ? 0 : v;
}

} // namespace flenser
