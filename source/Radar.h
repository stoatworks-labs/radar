#pragma once

#include "Audio.h"
#include "Controls.h"
#include "PassBuffer.h"
#include "Sweep.h"
#include "World.h"

#include <FFGLSDK.h>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

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

	/// The composite draws the harness a window onto the polar grid instead
	/// of the scope: output pixel ( x, y ) is texel ( x, y ) + origin, faded
	/// to now exactly as the scope's pixels are.
	void SetPolarViewForTest( bool on, int originBearing = 0, int originRange = 0 )
	{
		polarView      = on;
		polarOrigin[ 0 ] = originBearing;
		polarOrigin[ 1 ] = originRange;
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

private:
	void UpdateClock();
	bool ensureBuffers();
	void buildMap();
	void setBeamUniforms( ffglex::FFGLShader& shader ) const;

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
	int polarOrigin[ 2 ]      = { 0, 0 };
};

} // namespace radar
