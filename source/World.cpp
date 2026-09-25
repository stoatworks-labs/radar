#include "World.h"

#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace radar::world
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
/// The rain field's noise repeats every this many km (the shader's lattice is
/// 256 cells of 8 km), so the drift can be reduced without a jump.
constexpr double kRainPeriod = 2048.0;
/// km/s: rain cells drift with the wind, ~15 m/s.
constexpr double kRainDriftX = 0.012, kRainDriftY = -0.008;
} // namespace

//= mirrored in Shaders.cpp, pcg(). Integer only: the same on every GPU.
uint32_t Pcg( uint32_t v )
{
	const uint32_t state = v * 747796405u + 2891336453u;
	const uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

double Hash01( uint32_t v )
{
	return Pcg( v ) / 4294967296.0;
}

Contact Sea::spawn( uint32_t seed, uint32_t index, uint32_t generation, bool atRim ) const
{
	const uint32_t base = Pcg( seed * 7919u + index * 104729u + generation * 1299709u + 17u );
	auto h              = [ & ]( uint32_t salt ) { return Hash01( base + salt * 2654435761u ); };
	Contact c;
	c.generation = generation;
	const double bearing = 2.0 * kPi * h( 1 );
	double heading;
	if( atRim )
	{
		c.x     = std::sin( bearing );
		c.y     = std::cos( bearing );
		//Inward, within 40 degrees of the centre.
		heading = bearing + kPi + ( h( 2 ) - 0.5 ) * 1.4;
	}
	else
	{
		const double r = 0.2 + 0.75 * std::sqrt( h( 3 ) );
		c.x            = r * std::sin( bearing );
		c.y            = r * std::cos( bearing );
		heading        = 2.0 * kPi * h( 2 );
	}
	//0.003 to 0.012 scope units a second: at the default 24 rpm a contact
	//moves 3 to 10 pixels a rotation at 720p, so its past paints separate
	//into the plot of a track.
	const double speed = 0.003 + 0.009 * h( 4 );
	c.vx               = speed * std::sin( heading );
	c.vy               = speed * std::cos( heading );
	//A few big ones, mostly small: sigma 1 to 6, skewed low.
	c.sigma = static_cast< float >( 1.0 + 5.0 * h( 5 ) * h( 6 ) );
	return c;
}

void Sea::SetCount( int count, uint32_t seed )
{
	count = std::max( count, 0 );
	if( seed != lastSeed )
	{
		contacts.clear();
		lastSeed = seed;
	}
	while( static_cast< int >( contacts.size() ) > count )
		contacts.pop_back();
	while( static_cast< int >( contacts.size() ) < count )
		contacts.push_back( spawn( seed, static_cast< uint32_t >( contacts.size() ), 0, false ) );
}

void Sea::Step( double dt, uint32_t seed )
{
	for( size_t i = 0; i < contacts.size(); ++i )
	{
		Contact& c = contacts[ i ];
		c.x += c.vx * dt;
		c.y += c.vy * dt;
		if( c.x * c.x + c.y * c.y > 1.02 * 1.02 )
			c = spawn( seed, static_cast< uint32_t >( i ), c.generation + 1, true );
	}
	for( Contact& c : transients )
	{
		c.x += c.vx * dt;
		c.y += c.vy * dt;
		c.life -= dt;
	}
	transients.erase( std::remove_if( transients.begin(), transients.end(), []( const Contact& c ) { return c.life <= 0.0; } ),
	                  transients.end() );
}

void Sea::SpawnTransient( double antennaBearing, int direction, double rotationSeconds, uint32_t serial )
{
	const uint32_t base = Pcg( serial * 2246822519u + 3266489917u );
	auto h              = [ & ]( uint32_t salt ) { return Hash01( base + salt * 2654435761u ); };
	//20 to 60 degrees ahead of the sweep, so it is painted within a sixth of
	//a rotation of the hit.
	const double ahead = ( 20.0 + 40.0 * h( 1 ) ) * kPi / 180.0;
	const double b     = antennaBearing + ( direction >= 0 ? ahead : -ahead );
	const double r     = 0.3 + 0.6 * h( 2 );
	Contact c;
	c.x     = r * std::sin( b );
	c.y     = r * std::cos( b );
	c.vx    = 0.0;
	c.vy    = 0.0;
	c.sigma = 6.0f;
	c.life  = rotationSeconds > 0.0 ? 3.0 * rotationSeconds : 8.0;
	transients.push_back( c );
	if( transients.size() > 8 )
		transients.erase( transients.begin() );
}

void Sea::DriftRain( double dt )
{
	rainX = std::fmod( rainX + kRainDriftX * dt, kRainPeriod );
	rainY = std::fmod( rainY + kRainDriftY * dt, kRainPeriod );
}

std::vector< Target > Sea::Targets() const
{
	std::vector< Target > out;
	for( const auto* list : { &contacts, &transients } )
		for( const Contact& c : *list )
		{
			if( static_cast< int >( out.size() ) >= kMaxTargets )
				break;
			const double r = std::sqrt( c.x * c.x + c.y * c.y );
			if( r > 1.02 )
				continue;
			double bearing = std::atan2( c.x, c.y );
			if( bearing < 0.0 )
				bearing += 2.0 * kPi;
			out.push_back( { static_cast< float >( bearing ), static_cast< float >( r ), c.sigma } );
		}
	return out;
}

} // namespace radar::world
