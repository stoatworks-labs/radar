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
	double bearing;     ///< radians, where it comes in from (Closing)
	double factor;      ///< its speed against Sprite Speed (before Speed Spread scales it)
	double w1, w2, p1, p2;///< the speed wobble's two sines: rad/s and phase
	double weaves, p3;  ///< the weave: cycles per scope radius travelled, phase
	double arrive;      ///< 0..1: a latecomer's share of the arrival window (generation 0)
	double entry;       ///< -1..1: where a Transit track crosses, as a share of the band
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
	//Salts 9 and 10 are new since v0.2.0; the eight above are untouched, so
	//every v0.2.0 bearing, speed and wobble is the same draw.
	d.arrive = h( 9 );
	d.entry  = 2.0 * h( 10 ) - 1.0;
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

/// The shortest track on a path: one radius closing; Transit's chord at the
/// edge of the band.
double shortestTrack( Path path )
{
	return path == Path::Transit ? 2.0 * std::sqrt( 1.0 - kTransitBand * kTransitBand ) : 1.0;
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

double Swarm::EntryOf( uint32_t seed, uint32_t slot, uint32_t generation )
{
	return kTransitBand * drawOf( seed, slot, generation ).entry;
}

double Swarm::TrackLength( Path path, uint32_t seed, uint32_t slot, uint32_t generation )
{
	if( path != Path::Transit )
		return 1.0;
	const double x = EntryOf( seed, slot, generation );
	return 2.0 * std::sqrt( std::max( 0.0, 1.0 - x * x ) );
}

double Swarm::ArrivalWindow( const Settings& settings )
{
	//The fastest any sprite can go is Sprite Speed x 3^Spread (the top of
	//Speed Spread's range), surging by ( 1 + a ) at most (|w| <= 1). A sprite
	//that arrives at t >= 0 cannot have covered the shortest track before
	//t + shortest / that, which is longer than the whole window.
	const double fastest = std::pow( 3.0, std::clamp( settings.spread, 0.0, 1.0 ) ) * ( 1.0 + wobbleOf( settings ) );
	return kArrivalShare * shortestTrack( settings.path ) / fastest;
}

double Swarm::WaitOf( const Settings& settings, uint32_t slot, int nth, int of )
{
	return ArrivalWindow( settings ) * ( nth + drawOf( settings.seed, slot, 0 ).arrive ) / std::max( of, 1 );
}

double Swarm::entryOf( uint32_t seed, uint32_t slot, uint32_t generation ) const
{
	return wideForTest ? drawOf( seed, slot, generation ).entry : EntryOf( seed, slot, generation );
}

double Swarm::lengthOf( Path path, uint32_t seed, uint32_t slot, uint32_t generation ) const
{
	if( path != Path::Transit )
		return 1.0;
	const double x = entryOf( seed, slot, generation );
	return 2.0 * std::sqrt( std::max( 0.0, 1.0 - x * x ) );
}

int Swarm::Waiting() const
{
	int waiting = 0;
	for( const Slot& s : slots )
		waiting += s.wait > 0.0;
	return waiting;
}

void Swarm::Step( double dt, const Settings& settings )
{
	last            = settings;
	const int count = std::max( settings.count, 0 );
	while( static_cast< int >( slots.size() ) > count )
		slots.pop_back();
	if( static_cast< int >( slots.size() ) < count )
	{
		//New slots: enough of them on the scope at once to bring it up to
		//Start With (never fewer than none, never more than are new), each
		//part-way along its track by the golden ratio; the rest latecomers.
		//At Start With's default every one is on at once: v0.2.0's start.
		const int onScreen = static_cast< int >( slots.size() ) - Waiting();
		const int fresh    = count - static_cast< int >( slots.size() );
		const int placeNow = std::clamp( settings.startWith - onScreen, 0, fresh );
		const int late     = fresh - placeNow;
		for( int made = 0; made < fresh; ++made )
		{
			const uint32_t i = static_cast< uint32_t >( slots.size() );
			Slot s;
			s.path = settings.path;
			if( made < placeNow )
				s.travelled = StartOf( i ) * lengthOf( s.path, settings.seed, i, 0 );
			else if( naiveForTest )
				s.wait = shortestTrack( settings.path ) * ( made - placeNow + drawOf( settings.seed, i, 0 ).arrive ) / late;
			else
				s.wait = WaitOf( settings, i, made - placeNow, late );
			slots.push_back( s );
		}
	}
	if( !( dt > 0.0 ) )
		return;

	const double a = wobbleOf( settings );
	for( size_t i = 0; i < slots.size(); ++i )
	{
		Slot& s     = slots[ i ];
		double move = dt;//the part of this frame the sprite moves for
		if( s.wait > 0.0 )
		{
			//A latecomer counts its wait down at Sprite Speed: at 0 it holds.
			const double base = std::max( settings.speed, 0.0 );
			const double step = perFrameForTest ? base / 60.0 : base * dt;
			if( step < s.wait )
			{
				s.wait -= step;
				continue;
			}
			//It comes in at the rim at the moment its wait ran out, inside
			//this frame (step >= wait > 0, so base > 0), and moves for the rest
			//of it: the arrival is exact at any frame rate, like the motion.
			move        = perFrameForTest ? dt : dt - s.wait / base;
			s.wait      = 0.0;
			s.path      = settings.path;
			s.age       = 0.0;
			s.travelled = 0.0;
		}
		const Draw d  = drawOf( settings.seed, static_cast< uint32_t >( i ), s.generation );
		const double v = SpeedOf( settings, static_cast< uint32_t >( i ), s.generation );
		const double t0 = s.age, t1 = s.age + move;
		if( perFrameForTest )
			s.travelled += v / 60.0;
		else
			s.travelled += v * ( move + ( a > 0.0 ? a * ( wobbleIntegral( d, t1 ) - wobbleIntegral( d, t0 ) ) : 0.0 ) );
		s.age = t1;
		//At the end of its track (the centre, or the bottom rim): a new one at
		//the start, on the path the controls now say. What the frame carried
		//past the end is dropped -- under a frame of distance, never visible.
		if( s.travelled >= lengthOf( s.path, settings.seed, static_cast< uint32_t >( i ), s.generation ) )
		{
			++s.generation;
			s.age       = 0.0;
			s.travelled = 0.0;
			s.path      = settings.path;
		}
	}
}

std::vector< Placed > Swarm::Sprites() const
{
	std::vector< Placed > out;
	out.reserve( slots.size() );
	const double weave = 0.35 * ( 1.0 - std::clamp( last.stability, 0.0, 1.0 ) );//radians, at Stability 0
	const double sway  = kTransitWeave * ( 1.0 - std::clamp( last.stability, 0.0, 1.0 ) );//scope radii, at Stability 0
	for( size_t i = 0; i < slots.size(); ++i )
	{
		const Slot& s = slots[ i ];
		if( s.wait > 0.0 )
			continue;//a latecomer, not in range yet
		const Draw d  = drawOf( last.seed, static_cast< uint32_t >( i ), s.generation );
		Placed p;
		if( s.path == Path::Transit )
		{
			//Straight down the chord at x0, from the top rim to the bottom one.
			//The weave is across the track, scaled by 4 f ( 1 - f ) (f the share
			//of the track behind it), so it fades to nothing at the rim: with
			//|x0| <= 0.85 and an amplitude <= 0.15, ( |x0| + amplitude )^2 <= 1
			//keeps the weaving track inside the scope (AGENTS.md has the sum).
			const double x0     = entryOf( last.seed, static_cast< uint32_t >( i ), s.generation );
			const double half   = std::sqrt( std::max( 0.0, 1.0 - x0 * x0 ) );
			const double length = 2.0 * half;
			const double along  = std::min( s.travelled, length );
			const double f      = length > 0.0 ? along / length : 1.0;
			double x            = x0;
			if( sway > 0.0 )
				x += sway * 4.0 * f * ( 1.0 - f ) * std::sin( 2.0 * kPi * d.weaves * along + d.p3 );
			const double y = northForTest ? along - half : half - along;
			double b       = std::atan2( x, y );
			if( b < 0.0 )
				b += 2.0 * kPi;
			p.x        = x;
			p.y        = y;
			p.range    = std::hypot( x, y );
			p.bearing  = b;
			p.progress = f;
		}
		else
		{
			//In from the rim on its hashed bearing, weaving in bearing (v0.2.0's).
			double b = d.bearing;
			if( weave > 0.0 )
				b += weave * std::sin( 2.0 * kPi * d.weaves * s.travelled + d.p3 );
			b = std::fmod( b, 2.0 * kPi );
			if( b < 0.0 )
				b += 2.0 * kPi;
			p.range      = std::max( 0.0, 1.0 - s.travelled );
			p.bearing    = b;
			p.x          = p.range * std::sin( b );
			p.y          = p.range * std::cos( b );
			p.progress   = std::min( 1.0, s.travelled );
		}
		p.slot       = static_cast< uint32_t >( i );
		p.generation = s.generation;
		p.path       = s.path;
		out.push_back( p );
	}
	return out;
}

} // namespace radar::swarm
