#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace radar
{
namespace
{
double clamp01( float v )
{
	return std::clamp( static_cast< double >( v ), 0.0, 1.0 );
}
double geometric( float v, double low, double high )
{
	return low * std::pow( high / low, clamp01( v ) );
}
float inverseGeometric( double value, double low, double high )
{
	return static_cast< float >( std::log( std::clamp( value, low, high ) / low ) / std::log( high / low ) );
}
double linear( float v, double low, double high )
{
	return low + ( high - low ) * clamp01( v );
}
constexpr double kRpmLow = 1.0, kRpmHigh = 120.0;
} // namespace

const std::vector< unsigned int >& HostOrder( bool effect )
{
	auto build = []( bool over ) {
		std::vector< unsigned int > order = { PT_RPM, PT_BEAMWIDTH, PT_SIDELOBES, PT_DIRECTION,
			                                  PT_PULSE, PT_RANGE, PT_GAIN, PT_STC, PT_CLUTTER, PT_NOISE };
		if( over )
			order.push_back( PT_THRESHOLD );
		else
			order.insert( order.end(), { PT_CONTACTS, PT_RAIN, PT_LAND, PT_SEED } );
		order.insert( order.end(), { PT_AUDIO, PT_AUDIO_STROBE } );
		if( !over )
			order.push_back( PT_AUDIO_CONTACTS );
		order.insert( order.end(), { PT_PERSISTENCE, PT_FLASH, PT_PHOSPHOR, PT_RINGS, PT_BEARING_MARKS, PT_HEADING_LINE,
			                         PT_SCOPE_SIZE } );
		if( over )
			order.push_back( PT_MIX );
		for( unsigned int id = PT_ABOUT_TEXT; id < PT_COUNT; ++id )
			order.push_back( id );
		return order;
	};
	static const std::vector< unsigned int > source = build( false ), over = build( true );
	return effect ? over : source;
}

const char* GroupOf( unsigned int id )
{
	if( id <= PT_DIRECTION )
		return "Antenna";
	if( id <= PT_STC )
		return "Transmitter";
	if( id <= PT_THRESHOLD )
		return "Returns";
	if( id <= PT_AUDIO_CONTACTS )
		return "Audio";
	if( id <= PT_MIX )
		return "Scope";
	return "About";
}

double RpmFromParam( float v )
{
	//Exactly zero at the bottom: a stopped antenna paints nothing and the
	//picture fades, which is a thing an operator wants.
	if( v <= 0.0f )
		return 0.0;
	return geometric( v, kRpmLow, kRpmHigh );
}
float ParamFromRpm( double rpm )
{
	return rpm <= 0.0 ? 0.0f : inverseGeometric( rpm, kRpmLow, kRpmHigh );
}
double BeamwidthFromParam( float v )
{
	return geometric( v, 0.5, 20.0 );
}
float ParamFromBeamwidth( double degrees )
{
	return inverseGeometric( degrees, 0.5, 20.0 );
}
double SidelobesFromParam( float v )
{
	return clamp01( v );
}
double PulseFromParam( float v )
{
	return geometric( v, 0.05, 20.0 );
}
float ParamFromPulse( double microseconds )
{
	return inverseGeometric( microseconds, 0.05, 20.0 );
}
double RangeFromParam( float v )
{
	return geometric( v, 0.5, 200.0 );
}
float ParamFromRange( double km )
{
	return inverseGeometric( km, 0.5, 200.0 );
}
double GainDbFromParam( float v )
{
	return linear( v, -30.0, 50.0 );
}
float ParamFromGainDb( double db )
{
	return static_cast< float >( std::clamp( ( db + 30.0 ) / 80.0, 0.0, 1.0 ) );
}
double StcFromParam( float v )
{
	return linear( v, 0.0, 4.0 );
}
double ClutterFromParam( float v )
{
	return clamp01( v );
}
double NoiseFromParam( float v )
{
	return clamp01( v );
}
double RainFromParam( float v )
{
	return clamp01( v );
}
double LandFromParam( float v )
{
	return clamp01( v );
}
double ThresholdFromParam( float v )
{
	return clamp01( v );
}
double PersistenceFromParam( float v )
{
	return geometric( v, 0.1, 30.0 );
}
float ParamFromPersistence( double seconds )
{
	return inverseGeometric( seconds, 0.1, 30.0 );
}
double FlashFromParam( float v )
{
	return linear( v, 0.0, 4.0 );
}
double FlashTauFor( Phosphor phosphor )
{
	//The fast component, stretched from the phosphor's real microseconds to
	//something a 60 fps picture can show: the order of a few frames. P19 has
	//the least flash of the three, a green screen the most lingering one.
	switch( phosphor )
	{
	case Phosphor::P19: return 0.025;
	case Phosphor::Green: return 0.06;
	default: return 0.035;
	}
}

int OptionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

} // namespace radar
