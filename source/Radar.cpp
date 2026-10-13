#include "Radar.h"

#include "Diag.h"
#include "GLState.h"
#include "Shaders.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9), so it has to be asked for by name.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

using namespace ffglex;

namespace radar
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}

constexpr int kClockVotes       = 4;
/// Host seconds. A bigger forward step, or any backward one, is a jump -- a
/// clip trigger or a scrub -- and no time passes across it. One second, not
/// boreal's quarter: a radar at 2 fps should still turn.
constexpr double kMaxFrameDelta = 1.0;
/// Beyond this many bins either side the beam pattern is not summed: 3
/// half-widths of the Gaussian (0.2% of the peak), 8 when there are sidelobes
/// (past the second, at -35 dB two-way).
constexpr double kWindowGaussian = 3.0, kWindowSidelobes = 8.0;
constexpr int kMaxTaps           = 128;
/// km of land the beam passes through for its return to fall by e.
constexpr double kShadowKm = 1.5;
/// A sprite's reflectivity, against the synthetic coast's 0.25 to 1: as
/// bright as the brightest coast, so at the defaults a sprite reads like land
/// at any range, and Gain and STC treat it exactly as they treat land.
constexpr float kSpriteLevel = 1.0f;
/// Grow On Approach at 1: a sprite at the centre is this many times its size
/// at the rim, linearly in between.
constexpr double kSpriteGrowth = 3.0;

const char* const kDirectionNames[]   = { "Clockwise", "Anticlockwise" };
const char* const kPhosphorNames[]    = { "P7", "P19", "Green" };
const char* const kOrientationNames[] = { "Upright", "Face Centre" };

/// The y where sinc^4( y ) = 1/2: sinc( y ) = 2^-1/4. Newton, once.
double sincHalfPower()
{
	const double target = std::pow( 0.5, 0.25 );
	double y            = 1.0;
	for( int i = 0; i < 40; ++i )
	{
		const double f  = std::sin( y ) / y - target;
		const double df = ( y * std::cos( y ) - std::sin( y ) ) / ( y * y );
		y -= f / df;
	}
	return y;
}

const char* nameOf( unsigned int id )
{
	switch( id )
	{
	case PT_RPM: return "RPM";
	case PT_BEAMWIDTH: return "Beamwidth";
	case PT_SIDELOBES: return "Sidelobes";
	case PT_DIRECTION: return "Direction";
	case PT_PULSE: return "Pulse Length";
	case PT_RANGE: return "Range";
	case PT_GAIN: return "Gain";
	case PT_STC: return "STC";
	case PT_CLUTTER: return "Clutter";
	case PT_NOISE: return "Noise";
	case PT_CONTACTS: return "Contacts";
	case PT_RAIN: return "Rain";
	case PT_LAND: return "Land";
	case PT_SEED: return "Seed";
	case PT_THRESHOLD: return "Threshold";
	case PT_AUDIO: return "Audio";
	case PT_AUDIO_STROBE: return "Audio Strobe";
	case PT_AUDIO_CONTACTS: return "Audio Contacts";
	case PT_PERSISTENCE: return "Persistence";
	case PT_FLASH: return "Flash";
	case PT_PHOSPHOR: return "Phosphor";
	case PT_RINGS: return "Rings";
	case PT_BEARING_MARKS: return "Bearing Marks";
	case PT_HEADING_LINE: return "Heading Line";
	case PT_SCOPE_SIZE: return "Scope Size";
	case PT_MIX: return "Mix";
	case PT_SPRITE_FILE: return "Sprite File";
	case PT_SPRITE_THRESHOLD: return "Sprite Threshold";
	case PT_SPRITE_COUNT: return "Sprite Count";
	case PT_SPRITE_SIZE: return "Sprite Size";
	case PT_SPRITE_SPEED: return "Sprite Speed";
	case PT_SPEED_SPREAD: return "Speed Spread";
	case PT_STABILITY: return "Stability";
	case PT_ORIENTATION: return "Orientation";
	case PT_SPRITE_HUE: return "Hue";
	case PT_SPRITE_SATURATION: return "Saturation";
	case PT_SPRITE_GROW: return "Grow On Approach";
	default: return "?";
	}
}

/// The ring spacing: a round number of km giving four to seven rings.
double ringSpacingKm( double rangeKm )
{
	const double raw    = rangeKm / 5.0;
	const double decade = std::pow( 10.0, std::floor( std::log10( raw ) ) );
	for( double step : { 1.0, 2.0, 2.5, 5.0, 10.0 } )
		if( step * decade >= raw * 0.8 )
			return step * decade;
	return 10.0 * decade;
}

struct PhosphorLook
{
	float flash[ 3 ], after[ 3 ], overlay[ 3 ], face[ 3 ];
};

const PhosphorLook& lookOf( Phosphor p )
{
	//Linear light, before the tube's 1 - exp( -x ). P7's flash is the blue
	//of its ZnS:Ag layer, its afterglow the yellow-green of the (Zn,Cd)S:Cu
	//one; P19 is orange all through; Green is a single green.
	static const PhosphorLook looks[] = {
		{ { 0.35f, 0.50f, 1.10f }, { 0.95f, 1.10f, 0.28f }, { 0.35f, 0.42f, 0.18f }, { 0.004f, 0.009f, 0.006f } },
		{ { 1.60f, 0.85f, 0.30f }, { 1.30f, 0.55f, 0.08f }, { 0.45f, 0.26f, 0.06f }, { 0.009f, 0.005f, 0.002f } },
		{ { 0.60f, 1.60f, 0.70f }, { 0.20f, 1.20f, 0.30f }, { 0.12f, 0.45f, 0.16f }, { 0.002f, 0.010f, 0.004f } },
	};
	return looks[ static_cast< int >( p ) ];
}

/// The sprites' colour: the phosphor's, turned about the grey axis by Hue
/// (Rodrigues, so the mean of the three channels is kept) and its chroma
/// scaled by 2 x Saturation about that mean (0.5 keeps it). So the phosphor
/// still tints a sprite -- P7's two-tone flash and afterglow stay two-tone --
/// and Hue and Saturation move it from there.
void tintOf( const float in[ 3 ], double hueTurns, double saturation, float out[ 3 ] )
{
	const double a = 2.0 * kPi * hueTurns, c = std::cos( a ), s = std::sin( a );
	const double k    = 1.0 / std::sqrt( 3.0 );
	const double v[ 3 ] = { in[ 0 ], in[ 1 ], in[ 2 ] };
	const double mean = ( v[ 0 ] + v[ 1 ] + v[ 2 ] ) / 3.0;
	const double cross[ 3 ] = { k * ( v[ 2 ] - v[ 1 ] ), k * ( v[ 0 ] - v[ 2 ] ), k * ( v[ 1 ] - v[ 0 ] ) };
	for( int i = 0; i < 3; ++i )
	{
		const double turned = v[ i ] * c + cross[ i ] * s + mean * ( 1.0 - c );
		out[ i ]            = static_cast< float >( std::max( 0.0, mean + ( turned - mean ) * 2.0 * saturation ) );
	}
}
} // namespace

static_assert( PT_COUNT - PT_ABOUT_TEXT == stoatworks::about::kParamCount,
               "the About run no longer matches StoatworksAbout.h" );

//---------------------------------------------------------------------------
RadarPlugin::RadarPlugin( bool effect ) :
	isEffect( effect ),
	hostOrder( HostOrder( effect ) )
{
	SetMinInputs( isEffect ? 1 : 0 );
	SetMaxInputs( isEffect ? 1 : 0 );
	SetTimeSupported( true );

	std::fill( std::begin( idToHost ), std::end( idToHost ), -1 );
	for( size_t i = 0; i < hostOrder.size(); ++i )
		idToHost[ hostOrder[ i ] ] = static_cast< int >( i );

	//-------------------------------------------------------------------
	// Defaults. The source's are a harbour approach at dusk; the Over
	// effect's are the clip, whole, as the sea -- STC flat, so the far side
	// of the frame is not four orders of magnitude dimmer than the near.
	//-------------------------------------------------------------------
	params[ PT_RPM ]            = ParamFromRpm( 24.0 );
	params[ PT_BEAMWIDTH ]      = ParamFromBeamwidth( 2.0 );
	params[ PT_SIDELOBES ]      = 0.35f;
	params[ PT_DIRECTION ]      = 0.0f;
	params[ PT_PULSE ]          = ParamFromPulse( 2.0 );
	params[ PT_RANGE ]          = ParamFromRange( 24.0 );
	params[ PT_GAIN ]           = ParamFromGainDb( isEffect ? 8.0 : 0.0 );
	params[ PT_STC ]            = isEffect ? 1.0f : 0.65f;
	params[ PT_CLUTTER ]        = isEffect ? 0.2f : 0.4f;
	params[ PT_NOISE ]          = 0.3f;
	params[ PT_CONTACTS ]       = 7.0f;
	params[ PT_RAIN ]           = 0.4f;
	params[ PT_LAND ]           = 0.7f;
	params[ PT_SEED ]           = 11.0f;
	params[ PT_THRESHOLD ]      = 0.2f;
	params[ PT_AUDIO_STROBE ]   = 0.0f;
	params[ PT_AUDIO_CONTACTS ] = 0.0f;
	params[ PT_PERSISTENCE ]    = ParamFromPersistence( 1.6 );
	params[ PT_FLASH ]          = 0.375f;//1.5x the afterglow
	params[ PT_PHOSPHOR ]       = static_cast< float >( Phosphor::P7 );
	params[ PT_RINGS ]          = 0.35f;
	params[ PT_BEARING_MARKS ]  = 0.5f;
	params[ PT_HEADING_LINE ]   = isEffect ? 0.0f : 1.0f;
	params[ PT_SCOPE_SIZE ]     = isEffect ? 1.0f : 0.0f;
	params[ PT_MIX ]            = 1.0f;
	//Sprites (radar#5): OFF by default, so a composition saved with v0.1.0
	//(which stores only what differs from the defaults) looks as it did.
	params[ PT_SPRITE_FILE ]       = 0.0f;
	params[ PT_SPRITE_THRESHOLD ]  = 0.5f;
	params[ PT_SPRITE_COUNT ]      = 0.0f;
	params[ PT_SPRITE_SIZE ]       = ParamFromSpriteSize( 0.1 );
	params[ PT_SPRITE_SPEED ]      = ParamFromSpriteSpeed( 0.04 );
	params[ PT_SPEED_SPREAD ]      = 0.35f;
	params[ PT_STABILITY ]         = 0.75f;
	params[ PT_ORIENTATION ]       = static_cast< float >( Orientation::Upright );
	params[ PT_SPRITE_HUE ]        = 0.0f;
	params[ PT_SPRITE_SATURATION ] = 0.5f;
	params[ PT_SPRITE_GROW ]       = 0.0f;

	for( unsigned int host = 0; host < hostOrder.size(); ++host )
	{
		const unsigned int id = hostOrder[ host ];
		const char* name      = nameOf( id );
		switch( id )
		{
		case PT_DIRECTION:
			SetOptionParamInfo( host, name, 2, params[ id ] );
			for( int i = 0; i < 2; ++i )
				SetParamElementInfo( host, static_cast< unsigned int >( i ), kDirectionNames[ i ], static_cast< float >( i ) );
			break;
		case PT_PHOSPHOR:
			SetOptionParamInfo( host, name, 3, params[ id ] );
			for( int i = 0; i < 3; ++i )
				SetParamElementInfo( host, static_cast< unsigned int >( i ), kPhosphorNames[ i ], static_cast< float >( i ) );
			break;
		case PT_ORIENTATION:
			SetOptionParamInfo( host, name, 2, params[ id ] );
			for( int i = 0; i < 2; ++i )
				SetParamElementInfo( host, static_cast< unsigned int >( i ), kOrientationNames[ i ], static_cast< float >( i ) );
			break;
		case PT_SPRITE_FILE:
			//SetFileParamInfo ONLY: it pushes its own entry, and a SetParamInfo
			//for the same index registers it twice (gridiron's trap).
			SetFileParamInfo( host, name, { "png", "jpg", "jpeg" }, "" );
			break;
		case PT_SPRITE_COUNT:
			SetParamInfo( host, name, FF_TYPE_INTEGER, params[ id ] );
			SetParamRange( host, 0.0f, static_cast< float >( kMaxSprites ) );
			break;
		case PT_CONTACTS:
			//Only FF_TYPE_STANDARD has its default clamped into 0..1, so an
			//integer is declared with its real default and range.
			SetParamInfo( host, name, FF_TYPE_INTEGER, params[ id ] );
			SetParamRange( host, 0.0f, 16.0f );
			break;
		case PT_SEED:
			SetParamInfo( host, name, FF_TYPE_INTEGER, params[ id ] );
			SetParamRange( host, 0.0f, 9999.0f );
			break;
		case PT_HEADING_LINE:
			SetParamInfo( host, name, FF_TYPE_BOOLEAN, params[ id ] > 0.5f );
			break;
		case PT_AUDIO:
			//An FFT buffer: Resolume shows it as an audio-source picker.
			SetBufferParamInfo( host, name, audio::kBins, FF_USAGE_FFT );
			for( int i = 0; i < audio::kBins; ++i )
				SetParamElementInfo( host, static_cast< unsigned int >( i ), "", 0.0f );
			break;
		case PT_ABOUT_TEXT:
			SetParamInfo( host, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
			break;
		default:
			if( id > PT_ABOUT_TEXT )
				SetParamInfo( host, stoatworks::about::buttons()[ id - PT_ABOUT_TEXT - 1 ].label, FF_TYPE_EVENT, false );
			else
				SetParamInfo( host, name, FF_TYPE_STANDARD, params[ id ] );
			break;
		}
		SetParamGroup( host, GroupOf( id ) );
	}
}

RadarPlugin::~RadarPlugin() = default;

//---------------------------------------------------------------------------
FFResult RadarPlugin::InitGL( const FFGLViewportStruct* vp )
{
	diag::init();
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer="
	            + glStringOrUnknown( GL_RENDERER ) + " version=" + glStringOrUnknown( GL_VERSION ) );

	using namespace shaders;
	const std::string quadVertex = std::string( kVersion ) + kQuadVertex;
	struct Stage
	{
		FFGLShader* shader;
		std::string fragment;
		const char* name;
	};
	const Stage stages[] = {
		{ &kernelShader, Assemble( kKernelFragment ), "kernel" },
		{ &mapShader, Assemble( kMapFragment ), "map" },
		{ &reflectShader, Assemble( kReflectFragment ), "reflect" },
		{ &paintShader, Assemble( kPaintFragment ), "paint" },
		{ &compositeShader, Assemble( kCompositeFragment ), "composite" },
	};
	for( const Stage& stage : stages )
	{
		if( stage.shader->Compile( quadVertex.c_str(), stage.fragment.c_str() ) )
			continue;
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the plugin will do nothing" );
		FFGLLog::LogToHost( "Radar: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}
	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}
	diag::info( isEffect ? "initialised (Over)" : "initialised (source)" );
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
bool RadarPlugin::ensureBuffers()
{
	//All 32-bit float: a paint carries the last one's value forward through a
	//decay that can be within 1e-3 of 1, which half floats cannot represent
	//the distance from. Read texel by texel; the composite interpolates.
	//
	//Four components since radar#5: (flash, afterglow) of everything, then of
	//the sprites' share, so the composite can colour the sprites. The reflect
	//band carries (surface, sprites).
	bool ok = phosphor.Ensure( kBearings, kRanges, GL_RGBA32F, PassBuffer::Sampling::Nearest, PassBuffer::Wrap::Repeat );
	ok      = ok && scratch.Ensure( kBearings, kRanges, GL_RGBA32F, PassBuffer::Sampling::Nearest, PassBuffer::Wrap::Repeat );
	ok = ok && reflect.Ensure( kBearings, kRanges, GL_RG32F, PassBuffer::Sampling::Nearest, PassBuffer::Wrap::Repeat );
	ok = ok && kernel.Ensure( kBearings + 1, 1, GL_R32F, PassBuffer::Sampling::Nearest );
	if( !isEffect )
		ok = ok && map.Ensure( kMapSize, kMapSize, GL_RG32F, PassBuffer::Sampling::Linear );
	for( GLuint* texture : { &fadeTexture, &carryTexture } )
		if( *texture == 0 )
		{
			glGenTextures( 1, texture );
			glBindTexture( GL_TEXTURE_2D, *texture );
			glTexImage2D( GL_TEXTURE_2D, 0, GL_RG32F, kBearings, 1, 0, GL_RG, GL_FLOAT, nullptr );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
			glBindTexture( GL_TEXTURE_2D, 0 );
		}
	if( spriteTexture == 0 )
	{
		glGenTextures( 1, &spriteTexture );
		spriteTextureStale = true;
	}
	return ok && fadeTexture != 0 && carryTexture != 0 && spriteTexture != 0;
}

std::string RadarPlugin::spritePathNow()
{
	std::lock_guard< std::mutex > lock( spritePathMutex );
	return spritePath;
}

void RadarPlugin::updateSprite()
{
	const std::string path = spritePathNow();
	if( !spriteEverLoaded || path != spriteLoadedPath )
	{
		spriteEverLoaded = true;
		spriteLoadedPath = path;
		if( path.empty() )
		{
			spriteMask   = sprite::BuiltIn();
			spriteSource = SpriteSource::BuiltIn;
			spriteWhy.clear();
		}
		else
		{
			const sprite::Loaded loaded = sprite::LoadFile( path );
			if( loaded.ok )
			{
				spriteMask   = loaded.mask;
				spriteSource = SpriteSource::File;
				spriteWhy.clear();
				diag::info( "sprite file \"" + path + "\": " + std::to_string( loaded.sourceWidth ) + "x"
				            + std::to_string( loaded.sourceHeight ) + ", " + std::to_string( loaded.sourceChannels )
				            + " channels, mask " + std::to_string( spriteMask.width ) + "x" + std::to_string( spriteMask.height )
				            + " from its " + ( spriteMask.fromAlpha ? "alpha" : "brightest channel (no transparency)" ) );
			}
			else
			{
				//The path verbatim and quoted: a trailing space or a separator
				//the host rewrote survives a glance and not a log.
				diag::warn( "sprite file \"" + path + "\" " + loaded.why + " - using the built-in blip" );
				FFGLLog::LogToHost( "Radar: the sprite file could not be loaded, using the built-in blip (see the log)" );
				if( !spriteKeepLastForTest )
					spriteMask = sprite::BuiltIn();
				spriteSource = SpriteSource::Fallback;
				spriteWhy    = loaded.why;
			}
		}
		spriteTextureStale = true;
	}
	if( spriteTextureStale && spriteTexture != 0 )
		uploadSpriteMask();
}

void RadarPlugin::uploadSpriteMask()
{
	//The host's unpack state is its own: a bound unpack buffer would make this
	//read from it, and a row length or skip would shear the picture.
	GLint alignment = 4, rowLength = 0, skipRows = 0, skipPixels = 0, unpackBuffer = 0;
	glGetIntegerv( GL_UNPACK_ALIGNMENT, &alignment );
	glGetIntegerv( GL_UNPACK_ROW_LENGTH, &rowLength );
	glGetIntegerv( GL_UNPACK_SKIP_ROWS, &skipRows );
	glGetIntegerv( GL_UNPACK_SKIP_PIXELS, &skipPixels );
	glGetIntegerv( GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer );
	glBindBuffer( GL_PIXEL_UNPACK_BUFFER, 0 );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 4 );
	glPixelStorei( GL_UNPACK_ROW_LENGTH, 0 );
	glPixelStorei( GL_UNPACK_SKIP_ROWS, 0 );
	glPixelStorei( GL_UNPACK_SKIP_PIXELS, 0 );

	glBindTexture( GL_TEXTURE_2D, spriteTexture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_R32F, spriteMask.width, spriteMask.height, 0, GL_RED, GL_FLOAT, spriteMask.value.data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );

	glPixelStorei( GL_UNPACK_ALIGNMENT, alignment );
	glPixelStorei( GL_UNPACK_ROW_LENGTH, rowLength );
	glPixelStorei( GL_UNPACK_SKIP_ROWS, skipRows );
	glPixelStorei( GL_UNPACK_SKIP_PIXELS, skipPixels );
	glBindBuffer( GL_PIXEL_UNPACK_BUFFER, static_cast< GLuint >( unpackBuffer ) );
	spriteTextureStale = false;
}

void RadarPlugin::buildMap()
{
	const float rangeKm = static_cast< float >( RangeFromParam( params[ PT_RANGE ] ) );
	const float land    = static_cast< float >( LandFromParam( params[ PT_LAND ] ) );
	const unsigned seed = static_cast< unsigned >( std::clamp( std::lround( params[ PT_SEED ] ), 0L, 9999L ) );
	if( rangeKm == mapRange && land == mapLand && seed == mapSeed )
		return;
	mapRange = rangeKm;
	mapLand  = land;
	mapSeed  = seed;

	ScopedFBOBinding fbo( map.GetGLID(), ScopedFBOBinding::RB_REVERT );
	glViewport( 0, 0, kMapSize, kMapSize );
	ScopedShaderBinding shader( mapShader.GetGLID() );
	mapShader.Set( "RangeKm", rangeKm );
	mapShader.Set( "LandAmount", land );
	mapShader.Set( "MapSize", static_cast< float >( kMapSize ) );
	glUniform1ui( glGetUniformLocation( mapShader.GetGLID(), "MapSeed" ), world::Pcg( seed * 31u + 7u ) );
	quad.Draw();
}

void RadarPlugin::setBeamUniforms( FFGLShader& shader ) const
{
	static const double yHalf = sincHalfPower();
	const double half         = 0.5 * BeamwidthFromParam( params[ PT_BEAMWIDTH ] ) * kPi / 180.0;
	shader.Set( "BeamHalf", static_cast< float >( half ) );
	shader.Set( "SincK", static_cast< float >( yHalf / half ) );
	shader.Set( "Sidelobes", static_cast< float >( SidelobesFromParam( params[ PT_SIDELOBES ] ) ) );
	shader.Set( "OneWayForTest", oneWayForTest ? 1 : 0 );
}

//---------------------------------------------------------------------------
void RadarPlugin::UpdateClock()
{
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;
	const double raw = hostTime;

	//Resolume has been seen sending seconds and milliseconds through SetTime:
	//vote on the unit against the wall clock (rosette's code, via millpond).
	if( clockScale == 0.0 && raw >= 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;
			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
			{
				clockScale  = millisVotes > secondsVotes ? 0.001 : 1.0;
				settledJump = true;
			}
		}
	}
	if( raw >= 0.0 )
		lastRawTime = raw;
	lastWallTime = wallNow;
	now          = ( raw >= 0.0 && clockScale != 0.0 ) ? raw * clockScale : wallNow - wallStart;
}

//---------------------------------------------------------------------------
FFResult RadarPlugin::ProcessOpenGL( ProcessOpenGLStruct* pgl )
{
	if( pgl == nullptr )
		return FF_FAIL;
	const FFGLTextureStruct* input = nullptr;
	if( isEffect )
	{
		if( pgl->numInputTextures < 1 || pgl->inputTextures[ 0 ] == nullptr )
			return FF_FAIL;
		input = pgl->inputTextures[ 0 ];
	}

	ScopedGLState restore;
	const GLint* hostViewport = restore.saved.viewport;
	const int width           = input ? static_cast< int >( input->Width ) : hostViewport[ 2 ];
	const int height          = input ? static_cast< int >( input->Height ) : hostViewport[ 3 ];
	if( width <= 0 || height <= 0 )
		return FF_FAIL;
	glDisable( GL_BLEND );

	//-------------------------------------------------------------------
	// Time: the host's, as real elapsed seconds, frame to frame, in double.
	// A backwards or large step is a jump: no time passes across it, and the
	// audio analyser starts over (primed, so the first hit after it counts
	// and the loud frame it lands on does not).
	//-------------------------------------------------------------------
	UpdateClock();
	if( settledJump )
	{
		lastNow     = -1.0;
		settledJump = false;
	}
	double dt = 0.0;
	if( lastNow >= 0.0 )
	{
		const double step = floatClockForTest
		                        ? static_cast< double >( static_cast< float >( now ) - static_cast< float >( lastNow ) )
		                        : now - lastNow;
		if( step < 0.0 || step > kMaxFrameDelta )
			analyser.Reset();
		else
			dt = step;
	}
	lastNow = now;
	frameDt = dt;

	//-------------------------------------------------------------------
	// The antenna, in bins, in double; the wedge it swept.
	//-------------------------------------------------------------------
	const double rpm       = RpmFromParam( params[ PT_RPM ] );
	const int direction    = OptionIndex( params[ PT_DIRECTION ], 2 ) == 1 ? -1 : 1;
	const double rate      = rpm / 60.0 * kBearings * direction;
	const double b0        = antenna;
	antenna += rate * dt;
	const sweep::Wedge wedge = linesForTest ? sweep::LineAt( b0, antenna, dt ) : sweep::Crossed( b0, antenna, dt, kBearings );

	//-------------------------------------------------------------------
	// Audio: an onset is a strobe (both) and a new contact (source).
	//-------------------------------------------------------------------
	{
		float bins[ audio::kBins ] = {};
		int count                  = 0;
		if( const ParamInfo* info = FindParamInfo( static_cast< unsigned int >( idToHost[ PT_AUDIO ] ) ) )
		{
			count = static_cast< int >( std::min< size_t >( info->elements.size(), audio::kBins ) );
			for( int i = 0; i < count; ++i )
				bins[ i ] = info->elements[ static_cast< size_t >( i ) ].value;
		}
		audio::Settings settings;
		analyser.SetPrimingForTest( !unprimed );
		analyser.Update( bins, count, static_cast< float >( dt ), settings );
		const float strobe   = std::clamp( params[ PT_AUDIO_STROBE ], 0.0f, 1.0f );
		const float contacts = isEffect ? 0.0f : std::clamp( params[ PT_AUDIO_CONTACTS ], 0.0f, 1.0f );
		if( analyser.Fired() && ( strobe > 0.0f || contacts > 0.0f ) )
		{
			++eventsFired;
			if( strobe > 0.0f )
				pendingStrobe = std::max( pendingStrobe, 2.5f * strobe * std::max( analyser.Kick(), 0.3f ) );
			if( contacts > 0.0f )
				sea.SpawnTransient( ( antenna + 0.5 ) * 2.0 * kPi / kBearings, direction,
				                    rpm > 0.0 ? 60.0 / rpm : 0.0, ++transientSerial );
		}
	}

	//-------------------------------------------------------------------
	// The sea.
	//-------------------------------------------------------------------
	const unsigned seed = static_cast< unsigned >( std::clamp( std::lround( params[ PT_SEED ] ), 0L, 9999L ) );
	if( !isEffect )
	{
		sea.SetCount( std::clamp( static_cast< int >( std::lround( params[ PT_CONTACTS ] ) ), 0, 16 ), seed );
		sea.Step( dt, seed );
		sea.DriftRain( dt );
	}
	const std::vector< world::Target > targets = useTestTargets ? testTargets
	                                             : ( isEffect ? std::vector< world::Target >() : sea.Targets() );

	//-------------------------------------------------------------------
	// The sprites (radar#5), both plugins: in from the rim on the host's
	// clock. Placed below, once the mask (and so its aspect) is current.
	//-------------------------------------------------------------------
	{
		swarm::Settings settings;
		settings.count     = std::clamp( static_cast< int >( std::lround( params[ PT_SPRITE_COUNT ] ) ), 0, kMaxSprites );
		settings.seed      = spriteWallSeedForTest ? seed ^ static_cast< uint32_t >( wallSeconds() * 1.0e6 ) : seed;
		settings.speed     = SpriteSpeedFromParam( params[ PT_SPRITE_SPEED ] );
		settings.spread    = SpeedSpreadFromParam( params[ PT_SPEED_SPREAD ] );
		settings.stability = StabilityFromParam( params[ PT_STABILITY ] );
		swarm.Step( dt, settings );
	}

	//-------------------------------------------------------------------
	// The scope on the screen.
	//-------------------------------------------------------------------
	{
		const float inscribed = 0.95f * 0.5f * static_cast< float >( std::min( width, height ) );
		const float covering  = 0.5f * std::sqrt( static_cast< float >( width ) * width + static_cast< float >( height ) * height );
		scopeRadius           = inscribed + ( covering - inscribed ) * std::clamp( params[ PT_SCOPE_SIZE ], 0.0f, 1.0f );
		scopeCentre[ 0 ]      = 0.5f * static_cast< float >( width );
		scopeCentre[ 1 ]      = 0.5f * static_cast< float >( height );
	}

	//-------------------------------------------------------------------
	// Everything allocated before anything is bound.
	//-------------------------------------------------------------------
	if( !ensureBuffers() )
	{
		diag::error( "could not allocate the pass buffers" );
		return FF_FAIL;
	}
	if( ( width != lastWidth || height != lastHeight ) && lastWidth != 0 && clearOnResizeForTest )
		phosphor.Clear();
	lastWidth  = width;
	lastHeight = height;
	if( !isEffect )
		buildMap();
	updateSprite();
	{
		const double size      = SpriteSizeFromParam( params[ PT_SPRITE_SIZE ] );
		const double grow      = SpriteGrowFromParam( params[ PT_SPRITE_GROW ] );
		const bool faceCentre  = !spriteOrientationIgnoredForTest
		                        && OptionIndex( params[ PT_ORIENTATION ], static_cast< int >( Orientation::Count ) )
		                               == static_cast< int >( Orientation::FaceCentre );
		const double longer    = std::max( 1, std::max( spriteMask.width, spriteMask.height ) );
		const double aspectX   = spriteMask.width / longer, aspectY = spriteMask.height / longer;
		spriteDraws.clear();
		for( const swarm::Placed& placed : swarm.Sprites() )
		{
			const double side = size * ( 1.0 + kSpriteGrowth * grow * ( 1.0 - std::clamp( placed.range, 0.0, 1.0 ) ) );
			SpriteDraw d;
			d.x       = placed.x;
			d.y       = placed.y;
			d.hx      = 0.5 * side * aspectX;
			d.hy      = 0.5 * side * aspectY;
			//Face Centre: the sprite's up is its bearing, so its bottom faces
			//the centre whichever way it comes in.
			d.upSin   = faceCentre ? std::sin( placed.bearing ) : 0.0;
			d.upCos   = faceCentre ? std::cos( placed.bearing ) : 1.0;
			d.range   = placed.range;
			d.bearing    = placed.bearing;
			d.generation = placed.generation;
			spriteDraws.push_back( d );
		}
	}

	const double rangeKm     = RangeFromParam( params[ PT_RANGE ] );
	const double pulseExtent = pulseFactor * 2.0 * kMetresPerMicrosecond * PulseFromParam( params[ PT_PULSE ] ) / 1000.0 / rangeKm;
	const double beamHalf    = 0.5 * BeamwidthFromParam( params[ PT_BEAMWIDTH ] ) * kPi / 180.0;
	const double sidelobes   = SidelobesFromParam( params[ PT_SIDELOBES ] );
	const double binAngle    = 2.0 * kPi / kBearings;
	const int window = std::min( static_cast< int >( std::ceil( ( sidelobes > 0.0 ? kWindowSidelobes : kWindowGaussian ) * beamHalf / binAngle ) ),
	                             kBearings / 2 - 1 );
	const int stride = std::max( 1, ( window + kMaxTaps - 1 ) / kMaxTaps );

	//-------------------------------------------------------------------
	// 0. The beam's table, when the beam has changed.
	//-------------------------------------------------------------------
	if( static_cast< float >( beamHalf ) != kernelHalfBeam || static_cast< float >( sidelobes ) != kernelSidelobes
	    || oneWayForTest != kernelOneWay )
	{
		kernelHalfBeam  = static_cast< float >( beamHalf );
		kernelSidelobes = static_cast< float >( sidelobes );
		kernelOneWay    = oneWayForTest;
		ScopedFBOBinding fbo( kernel.GetGLID(), ScopedFBOBinding::RB_REVERT );
		glViewport( 0, 0, kBearings + 1, 1 );
		ScopedShaderBinding shader( kernelShader.GetGLID() );
		kernelShader.Set( "Half", kBearings / 2 );
		kernelShader.Set( "Bearings", kBearings );
		setBeamUniforms( kernelShader );
		quad.Draw();
	}

	//-------------------------------------------------------------------
	// 1. Reflectivity, for the band the beam reaches from this wedge. The
	// source with no land, or a clip thresholded at 1, has no surface at
	// all: nothing to resample.
	//-------------------------------------------------------------------
	const bool surfaceOn = isEffect ? ThresholdFromParam( params[ PT_THRESHOLD ] ) < 1.0 : LandFromParam( params[ PT_LAND ] ) > 0.0;
	//The sprites the beam can reach from this wedge: the band's bearings
	//against each sprite's bounding circle, so the shader loops over the one
	//or two in reach, not all of them.
	float spriteBox[ kMaxSprites * 4 ]   = {};
	float spriteTurn[ kMaxSprites * 4 ]  = {};
	float spriteReach[ kMaxSprites * 4 ] = {};
	spritesInBand                       = 0;
	if( wedge.count > 0 )
	{
		const int total      = wedge.count + 2 * window;
		const double bandMid = static_cast< double >( wedge.first - window ) + 0.5 * ( total - 1 );
		for( const SpriteDraw& d : spriteDraws )
		{
			if( spritesInBand >= kMaxSprites )
				break;
			const double rho = std::sqrt( d.hx * d.hx + d.hy * d.hy );
			if( d.range - rho >= 1.0 )
				continue;
			//The half-angle its bounding circle subtends from the centre, or
			//all the way round when the circle covers the centre.
			const double halfAngle = d.range <= rho ? kPi : std::asin( rho / d.range );
			bool reach             = total >= kBearings || d.range <= rho;
			if( !reach )
			{
				const double halfBins = halfAngle / binAngle;
				const double centre   = d.bearing / binAngle - 0.5;
				reach                 = std::fabs( std::remainder( centre - bandMid, static_cast< double >( kBearings ) ) )
				        <= 0.5 * ( total - 1 ) + halfBins + 1.0;
			}
			if( !reach )
				continue;
			float* box = &spriteBox[ spritesInBand * 4 ];
			box[ 0 ]   = static_cast< float >( d.x );
			box[ 1 ]   = static_cast< float >( d.y );
			box[ 2 ]   = static_cast< float >( d.hx );
			box[ 3 ]   = static_cast< float >( d.hy );
			spriteTurn[ spritesInBand * 4 + 0 ] = static_cast< float >( d.upSin );
			spriteTurn[ spritesInBand * 4 + 1 ] = static_cast< float >( d.upCos );
			//For the shader's per-texel test, a hair wider than the circle so
			//float rounding never cuts a sprite's edge.
			spriteReach[ spritesInBand * 4 + 0 ] = static_cast< float >( d.range );
			spriteReach[ spritesInBand * 4 + 1 ] = static_cast< float >( rho * 1.001 + 1e-5 );
			spriteReach[ spritesInBand * 4 + 2 ] = static_cast< float >( d.bearing );
			spriteReach[ spritesInBand * 4 + 3 ] = static_cast< float >( std::min( kPi, halfAngle * 1.001 + 1e-5 ) );
			++spritesInBand;
		}
	}
	const bool reflectOn = surfaceOn || spritesInBand > 0;
	if( wedge.count > 0 && reflectOn )
	{
		ScopedFBOBinding fbo( reflect.GetGLID(), ScopedFBOBinding::RB_REVERT );
		ScopedShaderBinding shader( reflectShader.GetGLID() );
		bindUnit( 0, isEffect ? input->Handle : map.TextureID() );
		reflectShader.Set( "MapTexture", 0 );
		reflectShader.Set( "IsEffect", isEffect ? 1 : 0 );
		reflectShader.Set( "Bearings", kBearings );
		reflectShader.Set( "Ranges", kRanges );
		reflectShader.Set( "PulseExtent", static_cast< float >( pulseExtent ) );
		reflectShader.Set( "ClipCentre", scopeCentre[ 0 ], scopeCentre[ 1 ] );
		reflectShader.Set( "ClipRadius", scopeRadius );
		reflectShader.Set( "MirrorForTest", mirrorForTest ? -1.0f : 1.0f );
		reflectShader.Set( "ClipRaster", static_cast< float >( width ), static_cast< float >( height ) );
		if( input )
		{
			const FFGLTexCoords maxCoords = GetMaxGLTexCoords( *input );
			reflectShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		}
		else
			reflectShader.Set( "MaxUV", 1.0f, 1.0f );
		reflectShader.Set( "Threshold", static_cast< float >( ThresholdFromParam( params[ PT_THRESHOLD ] ) ) );
		reflectShader.Set( "ShadowScale", static_cast< float >( rangeKm / kShadowKm ) );
		reflectShader.Set( "SurfaceOn", surfaceOn ? 1 : 0 );
		bindUnit( 1, spriteTexture );
		reflectShader.Set( "SpriteMask", 1 );
		reflectShader.Set( "SpriteCount", spritesInBand );
		glUniform4fv( glGetUniformLocation( reflectShader.GetGLID(), "SpriteBox" ), kMaxSprites, spriteBox );
		glUniform4fv( glGetUniformLocation( reflectShader.GetGLID(), "SpriteTurn" ), kMaxSprites, spriteTurn );
		glUniform4fv( glGetUniformLocation( reflectShader.GetGLID(), "SpriteReach" ), kMaxSprites, spriteReach );
		reflectShader.Set( "SpriteThreshold",
		                   spriteThresholdIgnoredForTest ? 0.0f : static_cast< float >( SpriteThresholdFromParam( params[ PT_SPRITE_THRESHOLD ] ) ) );

		const int total = wedge.count + 2 * window;
		if( total >= kBearings )
		{
			glViewport( 0, 0, kBearings, kRanges );
			quad.Draw();
		}
		else
		{
			const int start = sweep::Column( wedge.first - window, kBearings );
			const int first = std::min( total, kBearings - start );
			glViewport( start, 0, first, kRanges );
			quad.Draw();
			if( first < total )
			{
				glViewport( 0, 0, total - first, kRanges );
				quad.Draw();
			}
		}
		unbindTextureUnits( 2 );
	}

	//-------------------------------------------------------------------
	// 2. Time the paint, in double: when each crossed column was crossed,
	// the decay each carries from its last crossing, and every column's
	// fade from its last crossing to now.
	//-------------------------------------------------------------------
	const Phosphor kind = static_cast< Phosphor >( OptionIndex( params[ PT_PHOSPHOR ], 3 ) );
	const double tauF   = FlashTauFor( kind );
	const double tauA   = PersistenceFromParam( params[ PT_PERSISTENCE ] );
	{
		clock += dt;
		if( lastPaint.size() != static_cast< size_t >( kBearings ) )
		{
			lastPaint.assign( kBearings, -1.0e300 );
			fade.assign( static_cast< size_t >( kBearings ) * 2, 0.0f );
			carry.assign( static_cast< size_t >( kBearings ) * 2, 0.0f );
		}
		for( int i = 0; i < wedge.count; ++i )
		{
			const int col       = sweep::Column( wedge.first + i, kBearings );
			const double crossed = clock - std::max( ( wedge.b1Rel - i ) * wedge.ageScale, 0.0 );
			const double since   = crossed - lastPaint[ static_cast< size_t >( col ) ];
			const bool never     = lastPaint[ static_cast< size_t >( col ) ] < -1.0e299;
			carry[ static_cast< size_t >( col ) * 2 + 0 ] = !decayForTest ? 1.0f : never ? 0.0f : static_cast< float >( std::exp( -since / tauF ) );
			carry[ static_cast< size_t >( col ) * 2 + 1 ] = !decayForTest ? 1.0f : never ? 0.0f : static_cast< float >( std::exp( -since / tauA ) );
			lastPaint[ static_cast< size_t >( col ) ] = crossed;
		}
		for( int col = 0; col < kBearings; ++col )
		{
			const double age = clock - lastPaint[ static_cast< size_t >( col ) ];
			const bool never = lastPaint[ static_cast< size_t >( col ) ] < -1.0e299;
			fade[ static_cast< size_t >( col ) * 2 + 0 ] = !decayForTest ? 1.0f : never ? 0.0f : static_cast< float >( std::exp( -age / tauF ) );
			fade[ static_cast< size_t >( col ) * 2 + 1 ] = !decayForTest ? 1.0f : never ? 0.0f : static_cast< float >( std::exp( -age / tauA ) );
		}
		for( GLuint texture : { fadeTexture, carryTexture } )
		{
			glBindTexture( GL_TEXTURE_2D, texture );
			glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, kBearings, 1, GL_RG, GL_FLOAT,
			                 texture == fadeTexture ? fade.data() : carry.data() );
		}
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	//-------------------------------------------------------------------
	// 3. Paint the wedge into the scratch buffer, and copy its columns back.
	//-------------------------------------------------------------------
	if( wedge.count > 0 )
	{
		const int start = sweep::Column( wedge.first, kBearings );
		const int first = std::min( wedge.count, kBearings - start );
		const int rects[ 2 ][ 2 ] = { { start, first }, { 0, wedge.count - first } };

		ScopedFBOBinding fbo( scratch.GetGLID(), ScopedFBOBinding::RB_REVERT );
		{
			ScopedShaderBinding shader( paintShader.GetGLID() );
			bindUnit( 0, phosphor.TextureID() );
			bindUnit( 1, reflect.TextureID() );
			bindUnit( 2, carryTexture );
			bindUnit( 3, kernel.TextureID() );
			paintShader.Set( "Previous", 0 );
			paintShader.Set( "Reflect", 1 );
			paintShader.Set( "Carry", 2 );
			paintShader.Set( "Kernel", 3 );
			paintShader.Set( "KernelHalf", kBearings / 2 );
			paintShader.Set( "SurfaceOn", reflectOn ? 1 : 0 );
			paintShader.Set( "SpriteLevel", kSpriteLevel );
			paintShader.Set( "Bearings", kBearings );
			paintShader.Set( "Ranges", kRanges );
			paintShader.Set( "FirstCol", start );
			glUniform1ui( glGetUniformLocation( paintShader.GetGLID(), "FirstSerial" ),
			              static_cast< uint32_t >( static_cast< uint64_t >( wedge.first ) ) );
			paintShader.Set( "Weights", dropFlashForTest ? 0.0f : static_cast< float >( FlashFromParam( params[ PT_FLASH ] ) ), 1.0f );
			paintShader.Set( "CountForTest", countForTest ? 1 : 0 );
			paintShader.Set( "Window", window );
			paintShader.Set( "Stride", stride );
			setBeamUniforms( paintShader );
			paintShader.Set( "GainLinear", static_cast< float >( std::pow( 10.0, GainDbFromParam( params[ PT_GAIN ] ) / 10.0 ) ) );
			paintShader.Set( "Stc", static_cast< float >( StcFromParam( params[ PT_STC ] ) ) );
			paintShader.Set( "RangeExponent", rangeExponent );
			paintShader.Set( "PulseExtent", static_cast< float >( pulseExtent ) );

			float targetData[ kMaxTargets * 4 ] = {};
			const int targetCount = std::min( static_cast< int >( targets.size() ), kMaxTargets );
			for( int i = 0; i < targetCount; ++i )
			{
				targetData[ i * 4 + 0 ] = targets[ static_cast< size_t >( i ) ].bearing;
				targetData[ i * 4 + 1 ] = targets[ static_cast< size_t >( i ) ].range;
				targetData[ i * 4 + 2 ] = targets[ static_cast< size_t >( i ) ].sigma;
			}
			glUniform4fv( glGetUniformLocation( paintShader.GetGLID(), "Targets" ), kMaxTargets, targetData );
			paintShader.Set( "TargetCount", targetCount );

			const double clutter = ClutterFromParam( params[ PT_CLUTTER ] );
			paintShader.Set( "ClutterLevel", static_cast< float >( 1.2 * clutter ) );
			paintShader.Set( "ClutterKm", static_cast< float >( 0.6 + 4.0 * clutter ) );
			paintShader.Set( "NoiseLevel", static_cast< float >( 0.04 * NoiseFromParam( params[ PT_NOISE ] ) ) );
			paintShader.Set( "RainLevel", isEffect ? 0.0f : static_cast< float >( 0.25 * RainFromParam( params[ PT_RAIN ] ) ) );
			paintShader.Set( "RangeKm", static_cast< float >( rangeKm ) );
			paintShader.Set( "RainOffset", sea.RainOffsetX(), sea.RainOffsetY() );
			glUniform1ui( glGetUniformLocation( paintShader.GetGLID(), "Seed" ), world::Pcg( seed * 2654435761u + 99u ) );
			paintShader.Set( "StrobeLevel", pendingStrobe );
			pendingStrobe = 0.0f;
			for( const auto& rect : rects )
				if( rect[ 1 ] > 0 )
				{
					glViewport( rect[ 0 ], 0, rect[ 1 ], kRanges );
					quad.Draw();
				}
			unbindTextureUnits( 4 );
		}
		//The scratch buffer is the read framebuffer now: its wedge columns go
		//back over the phosphor's.
		glBindTexture( GL_TEXTURE_2D, phosphor.TextureID() );
		for( const auto& rect : rects )
			if( rect[ 1 ] > 0 )
				glCopyTexSubImage2D( GL_TEXTURE_2D, 0, rect[ 0 ], 0, rect[ 0 ], 0, rect[ 1 ], kRanges );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	//-------------------------------------------------------------------
	// 4. Composite, into the host's framebuffer and viewport.
	//-------------------------------------------------------------------
	glBindFramebuffer( GL_FRAMEBUFFER, pgl->HostFBO );
	glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );
	{
		const PhosphorLook& look = lookOf( static_cast< Phosphor >( OptionIndex( params[ PT_PHOSPHOR ], 3 ) ) );
		ScopedShaderBinding shader( compositeShader.GetGLID() );
		bindUnit( 0, phosphor.TextureID() );
		bindUnit( 1, fadeTexture );
		//A sampler bound to texture 0 is "unloadable" to Apple's GL (boreal's
		//trap); the source has no clip, so it gets any real texture.
		bindUnit( 2, input ? input->Handle : map.TextureID() );
		compositeShader.Set( "Phosphor", 0 );
		compositeShader.Set( "Fade", 1 );
		compositeShader.Set( "InputTexture", 2 );
		compositeShader.Set( "Bearings", kBearings );
		compositeShader.Set( "Ranges", kRanges );
		compositeShader.Set( "PolarViewForTest", polarView ? 1 : 0 );
		compositeShader.Set( "PolarSpritesForTest", polarSprites ? 1 : 0 );
		compositeShader.Set( "SpriteNoFadeForTest", spriteNoFadeForTest ? 1 : 0 );
		glUniform2i( glGetUniformLocation( compositeShader.GetGLID(), "PolarOrigin" ), polarOrigin[ 0 ], polarOrigin[ 1 ] );
		compositeShader.Set( "Raster", static_cast< float >( width ), static_cast< float >( height ) );
		compositeShader.Set( "ViewOrigin", static_cast< float >( hostViewport[ 0 ] ), static_cast< float >( hostViewport[ 1 ] ) );
		compositeShader.Set( "Centre", scopeCentre[ 0 ], scopeCentre[ 1 ] );
		compositeShader.Set( "RadiusPx", scopeRadius );
		if( input )
		{
			const FFGLTexCoords maxCoords = GetMaxGLTexCoords( *input );
			compositeShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		}
		else
			compositeShader.Set( "MaxUV", 1.0f, 1.0f );
		compositeShader.Set( "FlashColour", look.flash[ 0 ], look.flash[ 1 ], look.flash[ 2 ] );
		compositeShader.Set( "AfterColour", look.after[ 0 ], look.after[ 1 ], look.after[ 2 ] );
		compositeShader.Set( "OverlayColour", look.overlay[ 0 ], look.overlay[ 1 ], look.overlay[ 2 ] );
		compositeShader.Set( "FaceColour", look.face[ 0 ], look.face[ 1 ], look.face[ 2 ] );
		{
			//Hue 0 (or a whole turn) and Saturation 0.5 are the phosphor's own
			//colours, and then the composite does not touch the share at all.
			const double hue        = SpriteHueFromParam( params[ PT_SPRITE_HUE ] );
			const double saturation = SpriteSaturationFromParam( params[ PT_SPRITE_SATURATION ] );
			const bool tint         = ( hue > 0.0 && hue < 1.0 ) || saturation != 0.5;
			float flash[ 3 ] = { look.flash[ 0 ], look.flash[ 1 ], look.flash[ 2 ] };
			float after[ 3 ] = { look.after[ 0 ], look.after[ 1 ], look.after[ 2 ] };
			if( tint )
			{
				const double turn = spriteHueBackwardsForTest ? -hue : hue;
				tintOf( look.flash, turn, saturation, flash );
				tintOf( look.after, turn, saturation, after );
			}
			compositeShader.Set( "SpriteTint", tint ? 1 : 0 );
			compositeShader.Set( "SpriteFlashColour", flash[ 0 ], flash[ 1 ], flash[ 2 ] );
			compositeShader.Set( "SpriteAfterColour", after[ 0 ], after[ 1 ], after[ 2 ] );
		}
		compositeShader.Set( "RingSpacing", static_cast< float >( ringSpacingKm( rangeKm ) / rangeKm ) );
		compositeShader.Set( "RingLevel", std::clamp( params[ PT_RINGS ], 0.0f, 1.0f ) );
		compositeShader.Set( "MarkLevel", std::clamp( params[ PT_BEARING_MARKS ], 0.0f, 1.0f ) );
		compositeShader.Set( "HeadingLine", params[ PT_HEADING_LINE ] > 0.5f ? 1 : 0 );
		compositeShader.Set( "IsEffect", isEffect ? 1 : 0 );
		compositeShader.Set( "MixAmount", isEffect ? std::clamp( params[ PT_MIX ], 0.0f, 1.0f ) : 1.0f );
		quad.Draw();
		unbindTextureUnits( 3 );
	}
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult RadarPlugin::DeInitGL()
{
	for( FFGLShader* shader : { &kernelShader, &mapShader, &reflectShader, &paintShader, &compositeShader } )
		shader->FreeGLResources();
	quad.Release();
	phosphor.Destroy();
	scratch.Destroy();
	reflect.Destroy();
	for( GLuint* texture : { &fadeTexture, &carryTexture, &spriteTexture } )
		if( *texture )
		{
			glDeleteTextures( 1, texture );
			*texture = 0;
		}
	spriteTextureStale = true;
	map.Destroy();
	mapRange = -1.0f;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult RadarPlugin::SetTime( double time )
{
	hostTime = time;
	return FF_SUCCESS;
}

char* RadarPlugin::GetTextParameter( unsigned int index )
{
	if( index < hostOrder.size() && hostOrder[ index ] == PT_ABOUT_TEXT )
	{
		static const std::string text = stoatworks::about::textParam( 0 );
		return const_cast< char* >( text.c_str() );
	}
	if( index < hostOrder.size() && hostOrder[ index ] == PT_SPRITE_FILE )
	{
		//The host keeps the pointer past this call, and the path may be
		//replaced from another thread: hand back a copy that stays put.
		std::lock_guard< std::mutex > lock( spritePathMutex );
		spritePathReturn = spritePath;
		return const_cast< char* >( spritePathReturn.c_str() );
	}
	return CFFGLPlugin::GetTextParameter( index );
}

FFResult RadarPlugin::SetTextParameter( unsigned int index, const char* value )
{
	if( index < hostOrder.size() && hostOrder[ index ] == PT_ABOUT_TEXT )
		return FF_SUCCESS;
	//The host pushes the empty default through here on every new instance; a
	//failure would be the plugin refusing to load. The file is read later,
	//on the render thread (updateSprite), never here.
	if( index < hostOrder.size() && hostOrder[ index ] == PT_SPRITE_FILE )
	{
		std::lock_guard< std::mutex > lock( spritePathMutex );
		spritePath = value != nullptr ? value : "";
		return FF_SUCCESS;
	}
	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult RadarPlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= hostOrder.size() )
		return FF_FAIL;
	const unsigned int id = hostOrder[ index ];
	if( id >= PT_ABOUT_TEXT )
		return stoatworks::about::handleParam( id - PT_ABOUT_TEXT, value ) ? FF_SUCCESS : FF_FAIL;
	params[ id ] = value;
	return FF_SUCCESS;
}

float RadarPlugin::GetFloatParameter( unsigned int index )
{
	return index < hostOrder.size() ? params[ hostOrder[ index ] ] : 0.0f;
}

void RadarPlugin::SetById( unsigned int id, float value )
{
	const int host = HostIndexOf( id );
	if( host >= 0 )
		SetFloatParameter( static_cast< unsigned int >( host ), value );
}

void RadarPlugin::SetTargetsForTest( const std::vector< world::Target >& targets )
{
	testTargets    = targets;
	useTestTargets = true;
}

} // namespace radar
