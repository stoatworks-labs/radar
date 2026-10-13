#pragma once

#include <cstdint>
#include <vector>

/**
    The sprites' motion (radar#5): each one comes in from the rim of the scope
    to its centre, then a new one starts at the rim.

    Like the source's contacts, they live in SCOPE units (1 = the scope's
    radius), so Range zooms the coast and leaves them alone. Unlike those, they
    are not ships on straight tracks: every sprite heads for the centre, which
    is the picture radar#5 describes (skulls closing on the radar).

    **Deterministic.** A slot's every random choice is a PCG hash of (seed,
    slot, generation), and its progress is integrated in double from the
    host's real elapsed time, the antenna's clock -- so the same seed and the
    same clock give the same sprites, and `ratest` can render any frame twice.

    **Exact at any frame rate.** Speed wanders (Stability below 1) as
    v ( 1 + a w( age ) ), w a sum of two sines; the distance a frame adds is
    the closed-form integral of that over the frame, not v x dt at its start,
    so 24 fps and 240 fps put a sprite in the same place at the same time. The
    weave in bearing is a function of the distance travelled, so a sprite that
    is not moving does not sway.

    **A control never moves a sprite by itself.** Speed, Speed Spread and
    Stability set how fast the distance grows, never the distance: moving a
    slider changes where they are going, not where they are. (Seed is the
    exception: it re-hashes every bearing.)

    Count changes keep the slots that remain. A new slot -- at the first frame,
    or when Sprite Count is raised -- starts part-way in, staggered by the
    golden ratio, so the scope is populated at once and the sprites do not
    arrive in step; every later one starts at the rim.
*/
namespace radar::swarm
{
struct Settings
{
	int count          = 0;
	uint32_t seed      = 0;
	double speed       = 0.0;///< scope radii per second, the base
	double spread      = 0.0;///< 0..1: each sprite's own speed, 1/3x to 3x
	double stability   = 1.0;///< 0..1: 1 steady, 0 speed and bearing wander
};

/// One sprite this frame.
struct Placed
{
	double x = 0.0, y = 0.0;   ///< its centre, scope units, x east, y north
	double range   = 0.0;      ///< |centre|, 1 at the rim, 0 at the centre
	double bearing = 0.0;      ///< of its centre, radians clockwise from north, [0, 2 pi)
	uint32_t slot = 0, generation = 0;
};

class Swarm
{
public:
	/// Advance by dt seconds of host time, keeping `settings.count` slots.
	void Step( double dt, const Settings& settings );
	std::vector< Placed > Sprites() const;
	size_t Size() const
	{
		return slots.size();
	}

	/// The speed slot `slot` of generation `generation` has at Stability 1:
	/// Sprite Speed x its spread factor. For the harness.
	static double SpeedOf( const Settings& settings, uint32_t slot, uint32_t generation );
	/// Where a new slot starts: the fraction of the way in, [0, 1).
	static double StartOf( uint32_t slot );

	/// --sprite-motion's negative control: the distance a frame adds is
	/// v / 60, whatever the frame's real length -- motion tied to frames.
	void SetPerFrameForTest( bool on )
	{
		perFrameForTest = on;
	}

private:
	struct Slot
	{
		uint32_t generation = 0;
		double age          = 0.0;///< seconds since this generation started
		double travelled    = 0.0;///< scope radii in from the rim
	};

	std::vector< Slot > slots;
	Settings last;
	bool perFrameForTest = false;
};

} // namespace radar::swarm
