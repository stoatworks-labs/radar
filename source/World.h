#pragma once

#include <cstdint>
#include <vector>

/**
    The source's synthetic sea: moving contacts, and the drift of the rain.

    Contacts live in SCOPE units (1 is the edge of the scope, whatever Range
    says), not km: they are there to be seen, and a ship doing 20 knots on a
    100 km scope moves a pixel a minute. So Range zooms the land and the rain,
    which are in km, and leaves the traffic where it is. That is a decision,
    recorded in AGENTS.md, not physics.

    Everything is integrated in double from the host's real elapsed time, and
    every random choice is a PCG hash of (seed, contact, generation), so the
    same seed and the same clock give the same sea.
*/
namespace radar::world
{
uint32_t Pcg( uint32_t v );
double Hash01( uint32_t v );

struct Contact
{
	double x = 0.0, y = 0.0;  ///< scope units, x east, y north
	double vx = 0.0, vy = 0.0;///< scope units per second
	float sigma      = 1.0f;  ///< radar cross-section, relative
	uint32_t generation = 0;  ///< respawns so far
	double life      = -1.0;  ///< seconds left; < 0 lives for ever
};

/// A point target as the shader paints it.
struct Target
{
	float bearing;///< radians, clockwise from north
	float range;  ///< scope units
	float sigma;
};

class Sea
{
public:
	/// Keep `count` permanent contacts, spawning or dropping to match.
	void SetCount( int count, uint32_t seed );
	/// Advance every contact by dt seconds; respawn those that left.
	void Step( double dt, uint32_t seed );
	/// An audio onset: a transient contact just ahead of the sweep, living
	/// three rotations (or 8 s with the antenna stopped).
	void SpawnTransient( double antennaBearing, int direction, double rotationSeconds, uint32_t serial );
	/// The rain's drift, km, reduced into one noise period in double.
	void DriftRain( double dt );

	std::vector< Target > Targets() const;
	float RainOffsetX() const
	{
		return static_cast< float >( rainX );
	}
	float RainOffsetY() const
	{
		return static_cast< float >( rainY );
	}
	size_t Size() const
	{
		return contacts.size() + transients.size();
	}

private:
	Contact spawn( uint32_t seed, uint32_t index, uint32_t generation, bool atRim ) const;

	std::vector< Contact > contacts;
	std::vector< Contact > transients;
	uint32_t lastSeed = 0xffffffffu;
	double rainX = 0.0, rainY = 0.0;
};

} // namespace radar::world
