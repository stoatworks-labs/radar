#include "Swarm.h"

#include "World.h"

#include <algorithm>
#include <cmath>

namespace radar::swarm
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

/// The random choices of one slot's one generation, all from one PCG hash.
struct Draw
{
	double bearing;     ///< radians, where it comes in from
	double factor;      ///< its speed against Sprite Speed (before Speed Spread scales it)
	double w1, w2, p1, p2;///< the speed wobble's two sines: rad/s and phase
	double weaves, p3;  ///< the bearing weave: cycles per crossing, phase
};

Draw drawOf( uint32_t seed, uint32_t slot, uint32_t generation )
{
	const uint32_t base = world::Pcg( seed * 2246822519u + slot * 3266489917u + generation * 668265263u + 374761393u );
	auto h              = [ & ]( uint32_t salt ) { return world::Hash01( base + salt * 2654435761u ); };
	Draw d;
	d.bearing = 2.0 * kPi * h( 1 );
	d.factor  = 2.0 * h( 2 ) - 1.0;//-1..1: 3^( spread x this )
	d.w1      = 0.35 + 0.5 * h( 3 );
	d.w2      = 0.9 + 0.9 * h( 4 );
	d.p1      = 2.0 * kPi * h( 5 );
	d.p2      = 2.0 * kPi * h( 6 );
	d.weaves  = 1.5 + 1.5 * h( 7 );
	d.p3      = 2.0 * kPi * h( 8 );
	return d;
}

/// The wobble's amplitude: at Stability 0 the speed swings between 0.1x and
/// 1.9x its own, never stopping and never reversing.
double wobbleOf( const Settings& s )
{
	return 0.9 * ( 1.0 - std::clamp( s.stability, 0.0, 1.0 ) );
}

/// The integral of the wobble w( t ) = 0.6 sin( w1 t + p1 ) + 0.4 sin( w2 t + p2 ),
/// |w| <= 1: what a frame adds is exact, not a step.
double wobbleIntegral( const Draw& d, double t )
{
	return -0.6 * std::cos( d.w1 * t + d.p1 ) / d.w1 - 0.4 * std::cos( d.w2 * t + d.p2 ) / d.w2;
}
} // namespace

double Swarm::SpeedOf( const Settings& settings, uint32_t slot, uint32_t generation )
{
	const Draw d = drawOf( settings.seed, slot, generation );
	return std::max( settings.speed, 0.0 ) * std::pow( 3.0, std::clamp( settings.spread, 0.0, 1.0 ) * d.factor );
}

double Swarm::StartOf( uint32_t slot )
{
	const double golden = 0.6180339887498949;
	const double v      = ( slot + 0.5 ) * golden;
	return v - std::floor( v );
}

void Swarm::Step( double dt, const Settings& settings )
{
	last            = settings;
	const int count = std::max( settings.count, 0 );
	while( static_cast< int >( slots.size() ) > count )
		slots.pop_back();
	while( static_cast< int >( slots.size() ) < count )
	{
		Slot s;
		s.travelled = StartOf( static_cast< uint32_t >( slots.size() ) );
		slots.push_back( s );
	}
	if( !( dt > 0.0 ) )
		return;

	const double a = wobbleOf( settings );
	for( size_t i = 0; i < slots.size(); ++i )
	{
		Slot& s       = slots[ i ];
		const Draw d  = drawOf( settings.seed, static_cast< uint32_t >( i ), s.generation );
		const double v = SpeedOf( settings, static_cast< uint32_t >( i ), s.generation );
		const double t0 = s.age, t1 = s.age + dt;
		if( perFrameForTest )
			s.travelled += v / 60.0;
		else
			s.travelled += v * ( dt + ( a > 0.0 ? a * ( wobbleIntegral( d, t1 ) - wobbleIntegral( d, t0 ) ) : 0.0 ) );
		s.age = t1;
		//At the centre: a new one at the rim. What the frame carried past the
		//centre is dropped -- under a frame of distance, never visible.
		if( s.travelled >= 1.0 )
		{
			++s.generation;
			s.age       = 0.0;
			s.travelled = 0.0;
		}
	}
}

std::vector< Placed > Swarm::Sprites() const
{
	std::vector< Placed > out;
	out.reserve( slots.size() );
	const double weave = 0.35 * ( 1.0 - std::clamp( last.stability, 0.0, 1.0 ) );//radians, at Stability 0
	for( size_t i = 0; i < slots.size(); ++i )
	{
		const Slot& s = slots[ i ];
		const Draw d  = drawOf( last.seed, static_cast< uint32_t >( i ), s.generation );
		double b      = d.bearing;
		if( weave > 0.0 )
			b += weave * std::sin( 2.0 * kPi * d.weaves * s.travelled + d.p3 );
		b = std::fmod( b, 2.0 * kPi );
		if( b < 0.0 )
			b += 2.0 * kPi;
		Placed p;
		p.range      = std::max( 0.0, 1.0 - s.travelled );
		p.bearing    = b;
		p.x          = p.range * std::sin( b );
		p.y          = p.range * std::cos( b );
		p.slot       = static_cast< uint32_t >( i );
		p.generation = s.generation;
		out.push_back( p );
	}
	return out;
}

} // namespace radar::swarm
