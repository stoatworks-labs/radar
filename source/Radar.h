#pragma once

#include "Audio.h"
#include "Controls.h"
#include "PassBuffer.h"
#include "SpriteImage.h"
#include "Sweep.h"
#include "Swarm.h"
#include "World.h"

#include <FFGLSDK.h>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

#include <mutex>
#include <string>
#include <vector>

namespace radar
{
/**
    The plugin: the source (a synthetic sea) and, with `isEffect`, the Over
    effect (the clip is the sea). One class, two registrations; see
    SourcePlugin.cpp and EffectPlugin.cpp.

    Host indices are NOT ParamIds: each plugin declares its own dense list,
    `HostOrder( isEffect )`. The host-facing calls translate; everything
    inside works in ParamIds.
*/
class RadarPlugin : public CFFGLPlugin
{
public:
	/// Where the sprites' mask came from (radar#5).
	enum class SpriteSource
	{
		BuiltIn, ///< no file set: the blip
		File,    ///< the file named by Sprite File
		Fallback ///< the file failed (logged): the blip
	};

	/// One sprite as the reflect pass draws it, in scope units.
	struct SpriteDraw
	{
		double x = 0.0, y = 0.0;    ///< centre, x east, y north
		double hx = 0.0, hy = 0.0;  ///< half its width and height
		double upSin = 0.0, upCos = 1.0;///< its up, ( sin psi, cos psi )
		double range = 0.0, bearing = 0.0;
		uint32_t generation = 0;    ///< how many times this slot has started again at the rim
	};

	explicit RadarPlugin( bool isEffect );
	~RadarPlugin() override;

	FFResult InitGL( const FFGLViewportStruct* viewport ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* input ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	char* GetTextParameter( unsigned int index ) override;
	/// The base class's stub fails, and a failed default deletes the instance
	/// -- so without this no real host can load the plugin.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	FFResult SetTime( double time ) override;

	bool IsEffect() const
	{
		return isEffect;
	}
	unsigned int ParamCount() const
	{
		return static_cast< unsigned int >( hostOrder.size() );
	}
	/// The ParamId at a host index (the harness's --expect).
	unsigned int IdOfHostForTest( unsigned int host ) const
	{
		return host < hostOrder.size() ? hostOrder[ host ] : PT_COUNT;
	}
	/// The host index of a ParamId, or -1 if this plugin does not declare it.
	int HostIndexOf( unsigned int id ) const
	{
		return id < PT_COUNT ? idToHost[ id ] : -1;
	}
	/// Set by ParamId, as the host would through its index.
	void SetById( unsigned int id, float value );
	float GetById( unsigned int id ) const
	{
		return id < PT_COUNT ? params[ id ] : 0.0f;
	}

	//-------------------------------------------------------------------
	// For the harness. Nothing in the plugin's own operation calls these.
	//-------------------------------------------------------------------
	void SetClockScaleForTest( double scale )
	{
		clockScale = scale;
	}
	double ClockScale() const
	{
		return clockScale;
	}
	/// Paint exactly these point targets instead of the sea's contacts.
	void SetTargetsForTest( const std::vector< world::Target >& targets );
	/// false: nothing decays, a paint adds its echo at full weight. The
	/// geometry checks read what one rotation painted, undimmed.
	void SetDecayForTest( bool on )
	{
		decayForTest = on;
	}
	/// Every painted texel adds exactly 1: the state counts paints (--sweep).
	void SetCountForTest( bool on )
	{
		countForTest = on;
	}
	/// --sweep's negative control: a line per frame, not the swept wedge.
	void SetSweepAsLinesForTest( bool on )
	{
		linesForTest = on;
	}
	/// --r4's negative control: the radar equation's exponent (4).
	void SetRangeExponentForTest( float exponent )
	{
		rangeExponent = exponent;
	}
	/// --arc's negative control: the one-way pattern, sqrt 2 wider.
	void SetOneWayForTest( bool on )
	{
		oneWayForTest = on;
	}
	/// --pulse's negative control: the pulse's range extent is c tau x this
	/// (0.5 is right: the pulse goes out and comes back).
	void SetPulseFactorForTest( double factor )
	{
		pulseFactor = factor;
	}
	/// --persist's negative control: the flash is not painted at all.
	void SetDropFlashForTest( bool on )
	{
		dropFlashForTest = on;
	}
	/// --prime's negative control: the analyser deaf on its first frame too.
	void SetUnprimedForTest( bool on )
	{
		unprimed = on;
	}
	/// --clock's negative control: elapsed time taken from the host clock as
	/// a float, the way Resolume's ~499 million ms defeats.
	void SetFloatClockForTest( bool on )
	{
		floatClockForTest = on;
	}
	/// --over's negative control: the clip laid under the scope mirrored east
	/// for west, so its bearings run anticlockwise.
	void SetMirrorForTest( bool on )
	{
		mirrorForTest = on;
	}
	/// --resize's negative control: throw the phosphor away on a resize.
	void SetClearOnResizeForTest( bool on )
	{
		clearOnResizeForTest = on;
	}
	/// --sprites' negative control: Face Centre drawn upright.
	void SetSpriteOrientationIgnoredForTest( bool on )
	{
		spriteOrientationIgnoredForTest = on;
	}
	/// --sprite-threshold's negative control: any alpha at all is the target.
	void SetSpriteThresholdIgnoredForTest( bool on )
	{
		spriteThresholdIgnoredForTest = on;
	}
	/// --sprite-file's negative control: a file that fails leaves the last
	/// good picture up instead of falling back to the blip.
	void SetSpriteKeepLastOnFailureForTest( bool on )
	{
		spriteKeepLastForTest = on;
	}
	/// --sprite-beam's negative control: the sprites' share never fades.
	void SetSpriteNoFadeForTest( bool on )
	{
		spriteNoFadeForTest = on;
	}
	/// --sprite-speed's negative control: motion tied to frames, not time.
	void SetSpritePerFrameForTest( bool on )
	{
		swarm.SetPerFrameForTest( on );
	}
	/// --sprite-repeat's negative control: the sprites' seed salted from the
	/// wall clock, the way a rand() seeded with time() would be.
	void SetSpriteWallSeedForTest( bool on )
	{
		spriteWallSeedForTest = on;
	}
	/// --sprite-colour's negative control: Hue turns the colours backwards.
	void SetSpriteHueBackwardsForTest( bool on )
	{
		spriteHueBackwardsForTest = on;
	}

	/// The composite draws the harness a window onto the polar grid instead
	/// of the scope: output pixel ( x, y ) is texel ( x, y ) + origin, faded
	/// to now exactly as the scope's pixels are.
	/// `spriteShare`: the window shows the sprites' (flash, afterglow) share.
	void SetPolarViewForTest( bool on, int originBearing = 0, int originRange = 0, bool spriteShare = false )
	{
		polarView        = on;
		polarOrigin[ 0 ] = originBearing;
		polarOrigin[ 1 ] = originRange;
		polarSprites     = spriteShare;
	}

	/// The state: each texel as its column was left by its last paint.
	GLuint PhosphorTextureID() const
	{
		return phosphor.TextureID();
	}
	double AntennaBins() const
	{
		return antenna;
	}
	unsigned long long EventsFired() const
	{
		return eventsFired;
	}
	/// The scope, in output pixels, for the last frame: centre and radius.
	float ScopeCentreX() const
	{
		return scopeCentre[ 0 ];
	}
	float ScopeCentreY() const
	{
		return scopeCentre[ 1 ];
	}
	float ScopeRadius() const
	{
		return scopeRadius;
	}
	double LastDt() const
	{
		return frameDt;
	}
	size_t ContactCount() const
	{
		return sea.Size();
	}
	/// The sprites, as the last frame placed them (all of them, before the
	/// reflect pass culls to the band the beam can reach).
	const std::vector< SpriteDraw >& SpritesForTest() const
	{
		return spriteDraws;
	}
	SpriteSource SpriteSourceForTest() const
	{
		return spriteSource;
	}
	const std::string& SpriteWhyForTest() const
	{
		return spriteWhy;
	}
	const sprite::Mask& SpriteMaskForTest() const
	{
		return spriteMask;
	}
	/// How many sprites the last reflect pass was handed.
	int SpritesInBandForTest() const
	{
		return spritesInBand;
	}

private:
	void UpdateClock();
	bool ensureBuffers();
	void buildMap();
	void setBeamUniforms( ffglex::FFGLShader& shader ) const;
	/// Load the sprite file if Sprite File changed, and keep the texture
	/// current. Lazily, on the render thread: never in the constructor.
	void updateSprite();
	void uploadSpriteMask();
	std::string spritePathNow();

	const bool isEffect;
	const std::vector< unsigned int >& hostOrder;
	int idToHost[ PT_COUNT ];
	float params[ PT_COUNT ] = {};

	ffglex::FFGLShader kernelShader, mapShader, reflectShader, paintShader, compositeShader;
	ffglex::FFGLScreenQuad quad;

	PassBuffer phosphor, scratch;
	PassBuffer kernel;
	float kernelHalfBeam = -1.0f, kernelSidelobes = -1.0f;
	bool kernelOneWay    = false;
	PassBuffer reflect;
	PassBuffer map;
	GLuint fadeTexture = 0, carryTexture = 0;
	float mapRange = -1.0f, mapLand = -1.0f;
	unsigned mapSeed = 0xffffffffu;

	//The antenna, in bins, unwrapped, double (see Sweep.h).
	double antenna = -0.5;
	double frameDt = 0.0;
	//Seconds of elapsed host time since the instance started, double, and
	//when each bearing column was last crossed on that clock.
	double clock = 0.0;
	std::vector< double > lastPaint;
	std::vector< float > fade, carry;

	//Time. Resolume has sent both seconds and milliseconds (see millpond).
	double hostTime = -1.0, lastRawTime = -1.0, lastWallTime = -1.0, wallStart = -1.0;
	double clockScale = 0.0;
	int secondsVotes = 0, millisVotes = 0;
	double now = 0.0, lastNow = -1.0;
	bool settledJump = false;

	int lastWidth = 0, lastHeight = 0;
	float scopeCentre[ 2 ] = { 0.0f, 0.0f };
	float scopeRadius      = 1.0f;

	world::Sea sea;
	std::vector< world::Target > testTargets;
	bool useTestTargets = false;

	//Sprites (radar#5). The path arrives through SetTextParameter, possibly
	//from another thread, so it is kept under a lock; everything else is the
	//render thread's.
	std::mutex spritePathMutex;
	std::string spritePath, spritePathReturn;
	std::string spriteLoadedPath;
	bool spriteEverLoaded = false;
	sprite::Mask spriteMask;
	SpriteSource spriteSource = SpriteSource::BuiltIn;
	std::string spriteWhy;
	GLuint spriteTexture = 0;
	bool spriteTextureStale = true;
	swarm::Swarm swarm;
	std::vector< SpriteDraw > spriteDraws;
	int spritesInBand = 0;

	audio::Analyser analyser;
	bool unprimed       = false;
	float pendingStrobe = 0.0f;
	unsigned long long eventsFired = 0;
	uint32_t transientSerial = 0;

	bool decayForTest         = true;
	bool countForTest         = false;
	bool linesForTest         = false;
	float rangeExponent       = 4.0f;
	bool oneWayForTest        = false;
	double pulseFactor        = 0.5;
	bool dropFlashForTest     = false;
	bool floatClockForTest    = false;
	bool clearOnResizeForTest = false;
	bool mirrorForTest        = false;
	bool polarView            = false;
	bool polarSprites         = false;
	int polarOrigin[ 2 ]      = { 0, 0 };
	bool spriteOrientationIgnoredForTest = false;
	bool spriteThresholdIgnoredForTest   = false;
	bool spriteKeepLastForTest           = false;
	bool spriteNoFadeForTest             = false;
	bool spriteWallSeedForTest           = false;
	bool spriteHueBackwardsForTest       = false;
};

} // namespace radar
