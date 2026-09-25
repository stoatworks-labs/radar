/**
    ratest -- render Radar offline, and measure what its scope is doing.

    It drives the REAL plugin class, through the same ProcessOpenGL a host
    calls, on a synthetic clock, in a headless CGL context. The geometry
    checks read the phosphor the shipped shaders painted (range x bearing,
    the plugin's own state), and the screen checks read the output picture.

        ratest --out /tmp/scope.png       the source, the defaults
        ratest --over --out /tmp/o.png    the Over effect on the harness's card
        ratest --list                     every parameter and its default
        ratest --pipe [--frames N]        the source's frames, raw RGBA on stdout
        ratest --over --pipe              raw frames in, raw frames out
        ratest --film N                   N frames, raw RGBA on stdout
        ratest --offline                  the checks that need no GL context (CI)

    `--script` is the fleet's cue format: `frame  Parameter Name  value` lines,
    held before the first key and after the last. A STANDARD (0..1) control is
    linear between its keys; an option, a boolean, an event or an integer
    STEPS: it holds each key's value until the next key's frame. So a button
    press is three keys (0, 1, 0) and a dropdown never passes through the
    options between two keys.

    RATEST_RENDERER=software asks for Apple's software renderer by id, on a
    Mac with a GPU: what a GPU-less CI runner falls back to.

    The claims, one flag each -- see README "Building and testing".
*/

#include "Controls.h"
#include "Radar.h"
#include "Shaders.h"
#include "Sweep.h"
#include "World.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace radar;

namespace
{
constexpr double kPi  = 3.14159265358979323846;
constexpr double kUlp = 1.1920928955078125e-7;//2^-23, one float ulp at 1
/// The echo power a geometry check's target peaks at: small enough that the
/// video 1 - exp( -P ) is P to 2.5e-6 relative, large enough that a float
/// holds it to 1e-7.
constexpr double kPeakP = 1e-5;

using Floats = std::vector< float >;
using Bytes  = std::vector< unsigned char >;

//---------------------------------------------------------------------------
// Reporting.
//---------------------------------------------------------------------------
int g_failures = 0;
int g_checks   = 0;

std::string fmt( const char* format, ... )
{
	char buffer[ 4096 ];
	va_list args;
	va_start( args, format );
	std::vsnprintf( buffer, sizeof( buffer ), format, args );
	va_end( args );
	return buffer;
}

void Check( bool condition, const std::string& message )
{
	++g_checks;
	std::printf( "  %s  %s\n", condition ? "ok  " : "FAIL", message.c_str() );
	if( !condition )
		++g_failures;
}

int Verdict()
{
	std::printf( "\n  %s\n", g_failures == 0 ? "PASS" : "FAIL" );
	return g_failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// PNG. zlib ships with the OS.
//---------------------------------------------------------------------------
void putU32( Bytes& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( Bytes& out, const char* type, const Bytes& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

/// `rgba` is floats, row 0 at the BOTTOM (GL's order); the file is written top
/// row first, which is the only place anything here flips.
bool writePng( const std::string& path, int width, int height, const Floats& rgba )
{
	Bytes raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = height - 1; y >= 0; --y )
	{
		raw.push_back( 0 );
		for( int x = 0; x < width; ++x )
			for( int c = 0; c < 4; ++c )
			{
				const float v = rgba[ ( static_cast< size_t >( y ) * width + x ) * 4 + c ];
				raw.push_back( static_cast< unsigned char >( std::lround( std::clamp( v, 0.0f, 1.0f ) * 255.0f ) ) );
			}
	}
	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	Bytes compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	Bytes png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	Bytes ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.insert( ihdr.end(), { 8, 6, 0, 0, 0 } );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );
	FILE* file = std::fopen( path.c_str(), "wb" );
	if( !file )
		return false;
	const size_t written = std::fwrite( png.data(), 1, png.size(), file );
	std::fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The context. RATEST_RENDERER=software asks for Apple's software renderer
// by id (plotter's recipe, via wipe and repousse): what a GPU-less runner
// falls back to, and not repeatable at the last bit.
//---------------------------------------------------------------------------
bool g_software = false;

CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute fallback[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute generic[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFARendererID, static_cast< CGLPixelFormatAttribute >( kCGLRendererGenericFloatID ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	const char* renderer     = std::getenv( "RATEST_RENDERER" );
	if( renderer != nullptr && std::strcmp( renderer, "software" ) == 0 )
	{
		if( CGLChoosePixelFormat( generic, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
		g_software = true;
		std::fprintf( stderr, "ratest: RATEST_RENDERER=software, Apple's software renderer\n" );
	}
	else if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( fallback, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}
	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;
	CGLSetCurrentContext( context );
	return context;
}

const char* kindName( unsigned int type )
{
	switch( type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_STANDARD: return "standard";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_INTEGER: return "integer";
	default: return "other";
	}
}

/// A control whose value is a choice, a switch, a press or a count: cues
/// STEP between keys for these, and only a standard control ramps.
bool stepsBetweenCues( unsigned int type )
{
	return type == FF_TYPE_OPTION || type == FF_TYPE_BOOLEAN || type == FF_TYPE_EVENT || type == FF_TYPE_INTEGER;
}

//---------------------------------------------------------------------------
// The cue sheet.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( std::istream& in, const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::string line;
	int lineNumber = 0;
	while( std::getline( in, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream words( line );
		int frame = 0;
		if( !( words >> frame ) )
			continue;
		std::vector< std::string > parts;
		std::string word;
		while( words >> word )
			parts.push_back( word );
		if( parts.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}
		const float value = std::strtof( parts.back().c_str(), nullptr );
		parts.pop_back();
		std::string name = parts.front();
		for( size_t i = 1; i < parts.size(); ++i )
			name += " " + parts[ i ];
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::stable_sort( entry.second.begin(), entry.second.end(),
		                  []( const std::pair< int, float >& a, const std::pair< int, float >& b ) { return a.first < b.first; } );
	return tracks;
}

/// The value at `frame`: held before the first key and after the last; between
/// two keys linear if `ramp`, otherwise the earlier key's value until the
/// later key's frame.
float valueAt( const Track& track, int frame, bool ramp )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 0; i + 1 < track.size(); ++i )
	{
		const auto& a = track[ i ];
		const auto& b = track[ i + 1 ];
		if( frame >= a.first && frame < b.first )
		{
			if( !ramp )
				return a.second;
			const float t = static_cast< float >( frame - a.first ) / static_cast< float >( b.first - a.first );
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}

//---------------------------------------------------------------------------
// Pictures.
//---------------------------------------------------------------------------
double hash01( uint32_t a, uint32_t b = 0 )
{
	return world::Hash01( a * 2654435761u ^ world::Pcg( b + 0x9e3779b9u ) );
}

/// The Over effect's card: a harbour at night -- a dark sea, a lit waterfront,
/// boats, a bright ring. Rows bottom first (GL order).
Floats buildCard( int width, int height )
{
	Floats card( static_cast< size_t >( width ) * height * 4 );
	const double s = std::min( width, height );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double u = ( x + 0.5 - 0.5 * width ) / s, v = ( y + 0.5 - 0.5 * height ) / s;
			double lum     = 0.05 + 0.04 * ( 0.5 + v );
			//The waterfront: a band of lit blocks along the top.
			if( v > 0.28 && v < 0.42 )
			{
				const int block = static_cast< int >( std::floor( ( u + 2.0 ) * 18.0 ) );
				if( hash01( static_cast< uint32_t >( block ), 3 ) > 0.35 )
					lum = 0.55 + 0.45 * hash01( static_cast< uint32_t >( block ), 4 );
			}
			//Boats.
			for( int b = 0; b < 9; ++b )
			{
				const double bx = -0.7 + 1.4 * hash01( b, 11 ), by = -0.4 + 0.6 * hash01( b, 12 );
				const double r  = 0.012 + 0.02 * hash01( b, 13 );
				if( ( u - bx ) * ( u - bx ) * 0.3 + ( v - by ) * ( v - by ) < r * r )
					lum = 0.95;
			}
			//A ring.
			const double rr = std::sqrt( ( u + 0.35 ) * ( u + 0.35 ) + ( v + 0.25 ) * ( v + 0.25 ) );
			if( std::fabs( rr - 0.12 ) < 0.008 )
				lum = 0.9;
			float* o = &card[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
			o[ 0 ]   = static_cast< float >( lum * 0.95 );
			o[ 1 ]   = static_cast< float >( lum );
			o[ 2 ]   = static_cast< float >( lum * 1.05 );
			o[ 3 ]   = 1.0f;
		}
	return card;
}

GLuint makeTexture( int width, int height, const float* pixels, GLint format = GL_RGBA32F )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, format, width, height, 0, GL_RGBA, GL_FLOAT, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

//---------------------------------------------------------------------------
// Audio, written into the Audio buffer the way the host writes it.
//---------------------------------------------------------------------------
enum class AudioFeed
{
	Silence,
	Pulses///< a bass-heavy spectrum with a hit every half second
};

void feedAudio( RadarPlugin& plugin, double seconds, AudioFeed feed )
{
	const int host = plugin.HostIndexOf( PT_AUDIO );
	if( host < 0 )
		return;
	const double beat  = std::fmod( std::max( seconds, 0.0 ), 0.5 );
	const float strike = feed == AudioFeed::Pulses ? static_cast< float >( 0.15 + 1.5 * std::exp( -beat / 0.06 ) ) : 0.0f;
	for( int bin = 0; bin < audio::kBins; ++bin )
	{
		const float across = static_cast< float >( bin ) / static_cast< float >( audio::kBins - 1 );
		const float shape  = 0.7f * ( 1.0f - across ) * ( 1.0f - across ) + 0.2f * ( 0.5f + 0.5f * std::sin( 25.0f * across ) );
		plugin.SetParamElementValue( static_cast< unsigned int >( host ), static_cast< unsigned int >( bin ), shape * strike );
	}
}

//---------------------------------------------------------------------------
// A rig: the real plugin, a float output framebuffer, a synthetic clock.
//---------------------------------------------------------------------------
struct Rig
{
	RadarPlugin plugin;
	int width = 0, height = 0;
	GLuint sourceTexture = 0, outputTexture = 0, outputFBO = 0, readFBO = 0;
	int frame          = 0;
	double fps         = 60.0;
	double clockOffset = 0.0;
	/// The host's clock unit: 1 sends seconds, 1000 milliseconds.
	double hostUnit = 1.0;
	/// When set, frame n is clocked at times[ n ] seconds instead of n / fps.
	const std::vector< double >* times = nullptr;
	AudioFeed feed = AudioFeed::Silence;

	ProcessOpenGLStruct process    = {};
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };

	explicit Rig( bool effect = false ) : plugin( effect )
	{
	}

	~Rig()
	{
		plugin.DeInitGL();
		release();
		if( readFBO )
			glDeleteFramebuffers( 1, &readFBO );
	}

	void release()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	bool attach( int w, int h, const Floats* picture )
	{
		width         = w;
		height        = h;
		outputTexture = makeTexture( width, height, nullptr );
		glGenFramebuffers( 1, &outputFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0 );
		if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
			return false;
		process.HostFBO = outputFBO;
		if( plugin.IsEffect() )
		{
			const Floats card = picture ? *picture : buildCard( width, height );
			sourceTexture     = makeTexture( width, height, card.data() );
			inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
			inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
			inputStruct.Handle                              = sourceTexture;
			inputs[ 0 ]                                     = &inputStruct;
			process.numInputTextures                        = 1;
			process.inputTextures                           = inputs;
		}
		return true;
	}

	bool Init( int w, int h, const Floats* picture = nullptr, bool fixClock = true )
	{
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( w );
		viewport.height             = static_cast< FFUInt32 >( h );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see ~/Library/Logs/radar for which shader\n" );
			return false;
		}
		if( fixClock )
			plugin.SetClockScaleForTest( 1.0 );
		return attach( w, h, picture );
	}

	/// The host's raster changes under a running instance: no InitGL (Arena
	/// calls neither InitGL nor FF_RESIZE on a running clip).
	bool Resize( int w, int h, const Floats* picture = nullptr )
	{
		release();
		return attach( w, h, picture );
	}

	void Upload( const Floats& picture )
	{
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, picture.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	void Set( unsigned int id, float value )
	{
		plugin.SetById( id, value );
	}

	double TimeOf( int f ) const
	{
		if( times )
			return clockOffset + ( *times )[ static_cast< size_t >( f ) ];
		return clockOffset + static_cast< double >( f ) / fps;
	}

	bool Render( int frames = 1 )
	{
		for( int i = 0; i < frames; ++i )
		{
			const double seconds = TimeOf( frame );
			plugin.SetTime( seconds * hostUnit );
			feedAudio( plugin, seconds, feed );
			++frame;
			glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
			glViewport( 0, 0, width, height );
			glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
			glClear( GL_COLOR_BUFFER_BIT );
			if( plugin.ProcessOpenGL( &process ) != FF_SUCCESS )
			{
				std::fprintf( stderr, "ProcessOpenGL failed\n" );
				return false;
			}
		}
		return true;
	}

	Floats Output() const
	{
		Floats pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return pixels;
	}

	/// The whole phosphor: kRanges rows of kBearings (flash, afterglow) pairs.
	Floats Phosphor() const
	{
		Floats data( static_cast< size_t >( kBearings ) * kRanges * 2 );
		glBindTexture( GL_TEXTURE_2D, plugin.PhosphorTextureID() );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glGetTexImage( GL_TEXTURE_2D, 0, GL_RG, GL_FLOAT, data.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		return data;
	}

	/// One rectangle of the phosphor, (flash, afterglow) pairs, row by row.
	Floats PhosphorRect( int x, int y, int w, int h )
	{
		if( !readFBO )
			glGenFramebuffers( 1, &readFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, readFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, plugin.PhosphorTextureID(), 0 );
		Floats data( static_cast< size_t >( w ) * h * 2 );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( x, y, w, h, GL_RG, GL_FLOAT, data.data() );
		glBindFramebuffer( GL_FRAMEBUFFER, 0 );
		return data;
	}
};

float at( const Floats& phosphor, int bearing, int range, int channel )
{
	return phosphor[ ( static_cast< size_t >( range ) * kBearings + static_cast< size_t >( bearing ) ) * 2 + channel ];
}

/// Nothing but what a check puts there: no clutter, rain, noise, land,
/// traffic or graticule.
void quiet( Rig& rig )
{
	rig.Set( PT_CLUTTER, 0.0f );
	rig.Set( PT_NOISE, 0.0f );
	rig.Set( PT_RINGS, 0.0f );
	rig.Set( PT_BEARING_MARKS, 0.0f );
	rig.Set( PT_HEADING_LINE, 0.0f );
	rig.Set( PT_CONTACTS, 0.0f );
	rig.Set( PT_RAIN, 0.0f );
	rig.Set( PT_LAND, 0.0f );
	rig.Set( PT_AUDIO_STROBE, 0.0f );
	rig.Set( PT_AUDIO_CONTACTS, 0.0f );
}

double bearingOfBin( double k )
{
	return ( k + 0.5 ) * 2.0 * kPi / kBearings;
}

//---------------------------------------------------------------------------
// Parameters by display name.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	float value;
	unsigned int type;
};

std::vector< NamedParameter > listParameters( RadarPlugin& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < plugin.ParamCount(); ++i )
	{
		const char* const name = plugin.GetParamName( i );
		list.push_back( NamedParameter { name ? name : "?", i, plugin.GetFloatParameter( i ), plugin.GetParamType( i ) } );
	}
	return list;
}

bool applySetting( RadarPlugin& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name  = assignment.substr( 0, equals );
	const std::string value = assignment.substr( equals + 1 );
	for( const NamedParameter& parameter : listParameters( plugin ) )
		if( parameter.name == name )
		{
			plugin.SetFloatParameter( parameter.index, std::strtof( value.c_str(), nullptr ) );
			return true;
		}
	error = "no parameter called '" + name + "'";
	return false;
}

struct Cue
{
	Track track;
	bool ramp;
};

/// The cue sheet bound to a plugin's parameters, or an error naming the cue.
bool bindScript( RadarPlugin& plugin, const std::string& path, std::map< unsigned int, Cue >& out, std::string& error )
{
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return false;
	}
	const std::map< std::string, Track > tracks = loadScript( file, path, error );
	if( !error.empty() )
		return false;
	const std::vector< NamedParameter > known = listParameters( plugin );
	for( const auto& entry : tracks )
	{
		bool found = false;
		for( const NamedParameter& parameter : known )
			if( parameter.name == entry.first )
			{
				out[ parameter.index ] = Cue { entry.second, !stepsBetweenCues( parameter.type ) };
				found                  = true;
			}
		if( !found )
		{
			error = "script names '" + entry.first + "', which is not a parameter (try --list)";
			return false;
		}
	}
	return true;
}

//===========================================================================
// --pipe and --film. Raw RGBA, top row first, on the synthetic clock.
//===========================================================================
/// `readStdin`: the Over effect's frames come in on stdin, one out per one in,
/// until a partial frame or EOF. Otherwise frames are made -- `count` of them,
/// or, with `count` 0, until the reader hangs up (so that mode only ever ends
/// with exit 1; use a count for a take that can end cleanly).
int runPipe( bool effect, int width, int height, double fps, const std::string& scriptPath, int count, bool readStdin,
             bool beat, const std::vector< std::string >& settings )
{
	Rig rig( effect );
	rig.fps = fps;
	if( !rig.Init( width, height ) )
		return 1;
	if( beat )
		rig.feed = AudioFeed::Pulses;
	for( const std::string& setting : settings )
	{
		std::string error;
		if( !applySetting( rig.plugin, setting, error ) )
		{
			std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
			return 2;
		}
	}
	//A misspelt cue that silently did nothing would film a take that looks
	//deliberate and is wrong: refuse any name that is not a parameter.
	std::map< unsigned int, Cue > automation;
	if( !scriptPath.empty() )
	{
		std::string error;
		if( !bindScript( rig.plugin, scriptPath, automation, error ) )
		{
			std::fprintf( stderr, "%s\n", error.c_str() );
			return 2;
		}
	}

	Bytes in( static_cast< size_t >( width ) * height * 4 );
	Floats picture( in.size() );
	for( int index = 0; readStdin || count <= 0 || index < count; ++index )
	{
		if( readStdin )
		{
			size_t filled = 0;
			while( filled < in.size() )
			{
				const ssize_t got = read( STDIN_FILENO, in.data() + filled, in.size() - filled );
				if( got <= 0 )
					break;
				filled += static_cast< size_t >( got );
			}
			//A partial frame is the end of the stream, never a frame.
			if( filled < in.size() )
			{
				if( filled > 0 )
					std::fprintf( stderr, "partial frame at the end (%zu of %zu bytes): dropped\n", filled, in.size() );
				break;
			}
			for( int y = 0; y < height; ++y )
				for( int x = 0; x < width * 4; ++x )
					picture[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ] = in[ static_cast< size_t >( y ) * width * 4 + x ] / 255.0f;
			if( effect )
				rig.Upload( picture );
		}

		//Through the plugin's own setter, so a cue moves what a slider would.
		for( const auto& cue : automation )
			rig.plugin.SetFloatParameter( cue.first, valueAt( cue.second.track, index, cue.second.ramp ) );
		if( !rig.Render( 1 ) )
			return 1;

		const Floats out = rig.Output();
		Bytes bytes( in.size() );
		for( int y = 0; y < height; ++y )
			for( int x = 0; x < width * 4; ++x )
				bytes[ static_cast< size_t >( y ) * width * 4 + x ] = static_cast< unsigned char >(
					std::lround( std::clamp( out[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ], 0.0f, 1.0f ) * 255.0f ) );
		size_t written = 0;
		while( written < bytes.size() )
		{
			const ssize_t put = write( STDOUT_FILENO, bytes.data() + written, bytes.size() - written );
			//The reader has gone (`| head -c 1`, ffmpeg dying). SIGPIPE is
			//ignored in main(), so this is EPIPE and not a silent 141: say so
			//and stop, rather than render on into a closed pipe.
			if( put <= 0 )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				return 1;
			}
			written += static_cast< size_t >( put );
		}
	}
	return 0;
}

//===========================================================================
// --bench
//===========================================================================
int runBench()
{
	struct Size
	{
		int w, h;
		const char* name;
	};
	const Size sizes[] = { { 1280, 720, "720p" }, { 1920, 1080, "1080p" }, { 3840, 2160, "4K" } };
	std::printf( "\n=== bench: the defaults, after 120 frames of warm-up, each frame timed with glFinish both sides\n" );
	for( bool effect : { false, true } )
		for( const Size& size : sizes )
		{
			Rig rig( effect );
			if( !rig.Init( size.w, size.h ) )
				return 1;
			if( !rig.Render( 120 ) )
				return 1;
			glFinish();
			constexpr int kTimed = 120;
			std::vector< double > times;
			for( int f = 0; f < kTimed; ++f )
			{
				const auto start = std::chrono::steady_clock::now();
				if( !rig.Render( 1 ) )
					return 1;
				glFinish();
				times.push_back( std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count() );
			}
			double mean = 0.0;
			for( double t : times )
				mean += t / kTimed;
			std::sort( times.begin(), times.end() );
			std::printf( "  %-14s %-6s median %5.2f ms/frame, mean %5.2f, worst %5.2f  (median %4.1f%% of a 60 fps frame)\n",
			             effect ? "SW Radar Over" : "SW Radar", size.name, times[ kTimed / 2 ], mean, times.back(),
			             100.0 * times[ kTimed / 2 ] / ( 1000.0 / 60.0 ) );
		}
	return 0;
}

//===========================================================================
// The checks.
//
// Each takes a Perturb. With every field at its default the check scores the
// plugin against the physics; `--negative` sets one field at a time to a
// deliberately wrong MODEL (a test hook in the plugin, so the shipped shader
// computes the wrong thing) and requires the check to FAIL.
//===========================================================================
struct Perturb
{
	bool arcOneWay       = false;///< --arc: the plugin paints the one-way pattern
	bool pulseRoundTrip  = false;///< --pulse: the pulse extent is c tau, not c tau / 2
	bool sweepLines      = false;///< --sweep: a line per frame, not the swept wedge
	bool persistNoFlash  = false;///< --persist: the plugin paints no flash
	bool r4Dropped       = false;///< --r4: the radar equation without its R^-4
	bool overMirrored    = false;///< --over: the plugin lays the clip under the scope mirrored
	bool primeOff        = false;///< --prime: the analyser unprimed
	bool resizeClears    = false;///< --resize: the plugin clears the phosphor on a resize
	bool clockFloat      = false;///< --clock: elapsed time from a float host clock
	bool cuesRamp        = false;///< --cues: every control ramps between keys
	bool lawLines        = false;///< --sweep-law: Sweep.h's LineAt instead of Crossed
};

using CheckFn = int ( * )( const Perturb& );

/// The two rasters every pixel-reading check runs at: the one developed at,
/// and CI's.
struct Raster
{
	int w, h;
};
std::vector< Raster > kRasters = { { 1280, 720 }, { 320, 180 } };

//---------------------------------------------------------------------------
// Half-maximum width of a sampled profile around its peak, by linear
// interpolation between the samples either side of each crossing.
//---------------------------------------------------------------------------
struct Width
{
	double left = 0.0, right = 0.0, peak = 0.0;
	int peakAt   = 0;
	bool valid   = false;
};

Width halfMaxWidth( const std::vector< double >& profile )
{
	Width w;
	if( profile.size() < 3 )
		return w;
	int p = 0;
	for( int i = 1; i < static_cast< int >( profile.size() ); ++i )
		if( profile[ static_cast< size_t >( i ) ] > profile[ static_cast< size_t >( p ) ] )
			p = i;
	const double half = 0.5 * profile[ static_cast< size_t >( p ) ];
	int l             = p;
	while( l > 0 && profile[ static_cast< size_t >( l - 1 ) ] > half )
		--l;
	int r = p;
	while( r + 1 < static_cast< int >( profile.size() ) && profile[ static_cast< size_t >( r + 1 ) ] > half )
		++r;
	if( l == 0 || r + 1 == static_cast< int >( profile.size() ) )
		return w;
	const double a = profile[ static_cast< size_t >( l - 1 ) ], b = profile[ static_cast< size_t >( l ) ];
	const double c = profile[ static_cast< size_t >( r ) ], d = profile[ static_cast< size_t >( r + 1 ) ];
	w.left   = ( l - 1 ) + ( half - a ) / ( b - a );
	w.right  = r + ( c - half ) / ( c - d );
	w.peak   = profile[ static_cast< size_t >( p ) ];
	w.peakAt = p;
	w.valid  = true;
	return w;
}

/// The two-way pattern as the SPEC states it (not as the shader codes it):
/// Gaussian and uniform-aperture sinc^4, each 1/2 at +-B/2, mixed.
double patternAsStated( double off, double beamwidth, double sidelobes )
{
	const double h = 0.5 * beamwidth;
	const double g = std::pow( 0.5, ( off / h ) * ( off / h ) );
	//sinc( y )^4 = 1/2 at y = yh: bisect it here, independently of the plugin.
	static const double yh = [] {
		double lo = 0.5, hi = 1.5;
		for( int i = 0; i < 200; ++i )
		{
			const double mid = 0.5 * ( lo + hi );
			( std::pow( std::sin( mid ) / mid, 4.0 ) > 0.5 ? lo : hi ) = mid;
		}
		return 0.5 * ( lo + hi );
	}();
	const double x = yh * off / h;
	const double s = std::fabs( x ) < 1e-9 ? 1.0 : std::sin( x ) / x;
	return ( 1.0 - sidelobes ) * g + sidelobes * s * s * s * s;
}

/// Where linear interpolation between samples Delta apart can put a
/// half-maximum crossing wrong: |f''| Delta^2 / 8 over |f'|, at the crossing,
/// for the stated pattern, in units of Delta.
double interpolationBound( double beamwidth, double sidelobes, double delta )
{
	const double h = 0.5 * beamwidth, e = 1e-4 * h;
	double worstCurvature = 0.0;
	for( double x = h - delta; x <= h + delta; x += delta / 16.0 )
	{
		const double f2 = ( patternAsStated( x + e, beamwidth, sidelobes ) - 2.0 * patternAsStated( x, beamwidth, sidelobes )
		                    + patternAsStated( x - e, beamwidth, sidelobes ) ) / ( e * e );
		worstCurvature  = std::max( worstCurvature, std::fabs( f2 ) );
	}
	const double slope = std::fabs( patternAsStated( h + e, beamwidth, sidelobes ) - patternAsStated( h - e, beamwidth, sidelobes ) ) / ( 2.0 * e );
	return worstCurvature * delta * delta / 8.0 / slope / delta;
}

//===========================================================================
// --arc: a point target paints an arc as wide as the beam, at any range.
//===========================================================================
int runArc( const Perturb& perturb )
{
	std::printf( "\n=== arc: a point target's painted arc has a half-power width equal to Beamwidth, at two ranges\n" );
	const double binDeg = 360.0 / kBearings;
	for( const Raster& raster : kRasters )
		for( double beamwidth : { 2.0, 6.0 } )
			for( double sidelobes : { 0.0, 1.0 } )
			{
				Rig rig;
				if( !rig.Init( raster.w, raster.h ) )
					return 1;
				quiet( rig );
				rig.plugin.SetDecayForTest( false );
				rig.plugin.SetOneWayForTest( perturb.arcOneWay );
				rig.Set( PT_RPM, ParamFromRpm( 120.0 ) );//2048 bins in 30 frames
				rig.Set( PT_BEAMWIDTH, ParamFromBeamwidth( beamwidth ) );
				rig.Set( PT_SIDELOBES, static_cast< float >( sidelobes ) );
				rig.Set( PT_GAIN, ParamFromGainDb( -30.0 ) );
				rig.Set( PT_STC, 1.0f );//n = 4: flat, so the two ranges read the same
				rig.Set( PT_RANGE, ParamFromRange( 24.0 ) );
				//8 us: 1.2 km, 51 range bins, and 4.3 px deep even at 320x180, so
				//the screen reading below crosses a band, not a hairline.
				rig.Set( PT_PULSE, ParamFromPulse( 8.0 ) );
				const double ranges[] = { 0.3, 0.8 };
				const double pulseExtent = kMetresPerMicrosecond * 8.0 / 1000.0 / 24.0;
				const double bearings[] = { 37.31, 211.87 };//degrees: deliberately between bin centres
				std::vector< world::Target > targets;
				for( int i = 0; i < 2; ++i )
					targets.push_back( { static_cast< float >( bearings[ i ] * kPi / 180.0 ), static_cast< float >( ranges[ i ] ),
					                     static_cast< float >( kPeakP / 1e-3 ) } );
				rig.plugin.SetTargetsForTest( targets );
				if( !rig.Render( 31 ) )
					return 1;
				const Floats ph = rig.Phosphor();

				//Where the half-power crossing can land, in bins, per edge:
				//(1) linear interpolation between samples, |f''| Delta^2 / 8 over
				//|f'|, for the stated pattern; (2) the video, 1 - exp( -P ),
				//read in place of P: at the peak P_0 its half level sits P_0 / 4
				//low, relative, which moves the crossing by (P_0 / 8) / |f'|.
				const double delta     = 2.0 * kPi / kBearings;
				const double deltaBins = interpolationBound( beamwidth * kPi / 180.0, sidelobes, delta );
				const double hRad      = 0.5 * beamwidth * kPi / 180.0, e = 1e-6;
				const double slope     = std::fabs( patternAsStated( hRad + e, beamwidth * kPi / 180.0, sidelobes )
				                                    - patternAsStated( hRad - e, beamwidth * kPi / 180.0, sidelobes ) ) / ( 2.0 * e );
				const double videoBins = ( kPeakP / 8.0 ) / slope / delta;
				//(3) the shader's bearings are floats: the bin's and the target's,
				//each to 2 ulp at 2 pi (4.8e-7 rad), so an offset is good to
				//~1.2e-6 rad, 4e-4 bins, at each edge.
				const double floatBins = 1.2e-6 / delta;
				const double tolBins   = 2.0 * ( deltaBins + videoBins + floatBins );
				double widths[ 2 ] = { 0.0, 0.0 };
				for( int i = 0; i < 2; ++i )
				{
					const int row    = static_cast< int >( std::floor( ranges[ i ] * kRanges ) ) + 3;
					const double k0  = bearings[ i ] / binDeg - 0.5;//fractional bin of the target
					const int span   = static_cast< int >( std::ceil( 3.0 * beamwidth / binDeg ) );
					const int first  = static_cast< int >( std::floor( k0 ) ) - span;
					std::vector< double > profile;
					for( int k = first; k <= first + 2 * span + 1; ++k )
						profile.push_back( at( ph, sweep::Column( k, kBearings ), row, 1 ) );
					const Width w = halfMaxWidth( profile );
					widths[ i ]   = w.valid ? ( w.right - w.left ) * binDeg : 0.0;
					Check( w.valid && std::fabs( widths[ i ] - beamwidth ) <= tolBins * binDeg,
					       fmt( "%dx%d  B %.1f deg, sidelobes %.0f, R %.1f: FWHM %.5f deg (error %.2e bins, bound %.2e: "
					            "interpolation %.1e/edge, video %.1e, float angles %.1e)",
					            raster.w, raster.h, beamwidth, sidelobes, ranges[ i ], widths[ i ],
					            std::fabs( widths[ i ] - beamwidth ) / binDeg, tolBins, deltaBins, videoBins, floatBins ) );
				}

				//On the screen: the arc's LINEAR width grows with range. Sample
				//the output's luminance round the circle at each target's range
				//(bilinear between pixels) and take its half-power width in
				//pixels, against R x RadiusPx x Beamwidth.
				if( beamwidth == 6.0 && sidelobes == 0.0 )
				{
					const Floats out = rig.Output();
					const double cx = rig.plugin.ScopeCentreX(), cy = rig.plugin.ScopeCentreY(), radius = rig.plugin.ScopeRadius();
					auto luminance   = [ & ]( double x, double y ) {
						x -= 0.5;
						y -= 0.5;
						const int x0 = static_cast< int >( std::floor( x ) ), y0 = static_cast< int >( std::floor( y ) );
						const double fx = x - x0, fy = y - y0;
						double sum = 0.0;
						for( int dy = 0; dy < 2; ++dy )
							for( int dx = 0; dx < 2; ++dx )
							{
								const int xx = std::clamp( x0 + dx, 0, rig.width - 1 ), yy = std::clamp( y0 + dy, 0, rig.height - 1 );
								const float* p = &out[ ( static_cast< size_t >( yy ) * rig.width + xx ) * 4 ];
								const double l = 0.2126 * p[ 0 ] + 0.7152 * p[ 1 ] + 0.0722 * p[ 2 ];
								sum += l * ( dx ? fx : 1.0 - fx ) * ( dy ? fy : 1.0 - fy );
							}
						return sum;
					};
					double linear[ 2 ];
					bool ok = true;
					std::string what;
					for( int i = 0; i < 2; ++i )
					{
						const double rpx  = ( ranges[ i ] + 0.5 * pulseExtent ) * radius;//the middle of the streak
						const double step = 0.05 / rpx;//radians: a twentieth of a pixel along the arc
						const double b0   = bearings[ i ] * kPi / 180.0;
						std::vector< double > profile;
						const double reach = 3.0 * beamwidth * kPi / 180.0;
						for( double a = b0 - reach; a <= b0 + reach; a += step )
							profile.push_back( luminance( cx + rpx * std::sin( a ), cy + rpx * std::cos( a ) ) );
						const double floor = std::min( profile.front(), profile.back() );
						for( double& v : profile )
							v -= floor;
						const Width w       = halfMaxWidth( profile );
						linear[ i ]         = w.valid ? ( w.right - w.left ) * step * rpx : 0.0;
						const double expect = rpx * beamwidth * kPi / 180.0;
						//Half a pixel: the bilinear reconstruction here is a
						//tent of variance 1/6 px^2, which widens a Gaussian of
						//FWHM w to sqrt( w^2 + 8 ln2 / 6 ) -- +0.17 px at the
						//narrowest (2.7 px, 320x180) -- plus the pixel grid's
						//phase under the arc.
						const bool good = w.valid && std::fabs( linear[ i ] - expect ) <= 0.5;
						ok              = ok && good;
						what += fmt( " R %.1f: %.2f px (R x B = %.2f)", ranges[ i ], linear[ i ], expect );
					}
					Check( ok && linear[ 1 ] > linear[ 0 ],
					       fmt( "%dx%d  on the screen, the linear width grows with R, to half a pixel:%s", raster.w, raster.h, what.c_str() ) );
				}
			}
	return Verdict();
}

//===========================================================================
// --pulse: its radial extent is c tau / 2, starting at the target.
//===========================================================================
int runPulse( const Perturb& perturb )
{
	std::printf( "\n=== pulse: a point target's radial streak is c tau / 2 long, from its range outward\n" );
	for( const Raster& raster : kRasters )
		for( double pulse : { 1.0, 4.0 } )
		{
			Rig rig;
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			quiet( rig );
			rig.plugin.SetDecayForTest( false );
			rig.plugin.SetPulseFactorForTest( perturb.pulseRoundTrip ? 1.0 : 0.5 );
			rig.Set( PT_RPM, ParamFromRpm( 120.0 ) );
			//8 degrees: 4.7 px across at 320x180, so the screen reading below
			//runs along a band, not a hairline.
			rig.Set( PT_BEAMWIDTH, ParamFromBeamwidth( 8.0 ) );
			rig.Set( PT_SIDELOBES, 0.0f );
			rig.Set( PT_GAIN, ParamFromGainDb( -30.0 ) );
			rig.Set( PT_STC, 1.0f );
			const double rangeKm = 12.0;
			rig.Set( PT_RANGE, ParamFromRange( rangeKm ) );
			rig.Set( PT_PULSE, ParamFromPulse( pulse ) );
			const double rangeT = 0.40137, bearingT = 100.3;
			rig.plugin.SetTargetsForTest( { { static_cast< float >( bearingT * kPi / 180.0 ), static_cast< float >( rangeT ), 1.0f } } );
			if( !rig.Render( 31 ) )
				return 1;
			const Floats ph = rig.Phosphor();
			const int column = sweep::Column( static_cast< int64_t >( std::lround( bearingT / ( 360.0 / kBearings ) - 0.5 ) ), kBearings );
			double peak = 0.0, sum = 0.0;
			int firstLit = -1;
			for( int i = 0; i < kRanges; ++i )
			{
				const double v = at( ph, column, i, 1 );
				peak           = std::max( peak, v );
				sum += v;
				if( v > 0.0 && firstLit < 0 )
					firstLit = i;
			}
			//Each bin holds sigma x (the fraction of it the pulse covers):
			//the fractions add to the pulse's length exactly, so the sum over
			//the peak is the length in bins.
			const double extentKm = sum / peak / kRanges * rangeKm;
			const double wantKm   = kMetresPerMicrosecond * pulse / 1000.0;
			//Bound: the partial end bins read 1 - exp( -P f ) against f ( 1 -
			//exp( -P ) ): relative P ( 1 - f ) / 2 <= 5e-4 of one bin at P =
			//1e-3, twice; float sums of ~50 terms, 1e-6. So 2e-3 bins.
			const double tolKm = 2e-3 * rangeKm / kRanges;
			Check( std::fabs( extentKm - wantKm ) <= tolKm,
			       fmt( "%dx%d  %.0f us: extent %.6f km, c tau / 2 = %.6f km (error %.2e bins, bound 2e-3)", raster.w, raster.h, pulse,
			            extentKm, wantKm, std::fabs( extentKm - wantKm ) / ( rangeKm / kRanges ) ) );
			//Where it starts: the first lit bin holds the part of it past the
			//target, so the target's range is that bin's top minus its share.
			const double lead = firstLit >= 0 ? ( firstLit + 1 - at( ph, column, firstLit, 1 ) / peak ) / kRanges : -1.0;
			Check( std::fabs( lead - rangeT ) <= 2e-3 / kRanges,
			       fmt( "%dx%d  %.0f us: the streak starts at %.6f (the target is at %.6f) and runs OUTWARD", raster.w, raster.h, pulse, lead, rangeT ) );

			//On the screen, along the target's radial: the streak's length in
			//pixels is c tau / 2 at the display's scale.
			if( pulse == 4.0 )
			{
				const Floats out = rig.Output();
				const double cx = rig.plugin.ScopeCentreX(), cy = rig.plugin.ScopeCentreY(), radius = rig.plugin.ScopeRadius();
				const double b  = bearingOfBin( column );
				double s = 0.0, top = 0.0, base = 1e9;
				const double step = 0.05;
				std::vector< double > line;
				for( double d = 0.2 * radius; d <= 0.7 * radius; d += step )
				{
					const double x = cx + d * std::sin( b ) - 0.5, y = cy + d * std::cos( b ) - 0.5;
					const int x0 = static_cast< int >( std::floor( x ) ), y0 = static_cast< int >( std::floor( y ) );
					const double fx = x - x0, fy = y - y0;
					double l = 0.0;
					for( int dy = 0; dy < 2; ++dy )
						for( int dx = 0; dx < 2; ++dx )
						{
							const float* p = &out[ ( static_cast< size_t >( y0 + dy ) * rig.width + ( x0 + dx ) ) * 4 ];
							l += ( 0.2126 * p[ 0 ] + 0.7152 * p[ 1 ] + 0.0722 * p[ 2 ] ) * ( dx ? fx : 1 - fx ) * ( dy ? fy : 1 - fy );
						}
					line.push_back( l );
					base = std::min( base, l );
				}
				for( double l : line )
				{
					s += ( l - base ) * step;
					top = std::max( top, l - base );
				}
				const double px   = s / top;
				const double want = wantKm / rangeKm * radius;
				//Half a pixel: the integral survives bilinear sampling; the peak
				//of a streak 4 px long or more does not drop under a 1 px tent.
				Check( std::fabs( px - want ) <= 0.5,
				       fmt( "%dx%d  on the screen: %.2f px along the radial, c tau / 2 at the scale is %.2f px (bound 0.5 px)", raster.w,
				            raster.h, px, want ) );
			}
		}
	return Verdict();
}

//===========================================================================
// --sweep: every bearing painted once a rotation, at any frame rate.
//===========================================================================
int runSweep( const Perturb& perturb )
{
	std::printf( "\n=== sweep: after a rotation every bearing bin has been painted exactly once -- no gaps, no double paints\n" );
	struct Case
	{
		const char* what;
		double rpm, fps;
		int direction;
		bool jitter;
		int frames;
	};
	const Case cases[] = {
		{ "25 rpm at 60 fps: 144 frames a rotation (whole), 14.2 bins a frame", 25.0, 60.0, 0, false, 190 },
		{ "23 rpm at 60 fps: 156.52 frames a rotation (fractional)", 23.0, 60.0, 0, false, 200 },
		{ "24 rpm at 59.94 fps (fractional)", 24.0, 59.94, 0, false, 190 },
		{ "37 rpm, frames 8-30 ms apart at random (real elapsed time)", 37.0, 60.0, 0, true, 170 },
		{ "23 rpm anticlockwise", 23.0, 60.0, 1, false, 200 },
		{ "1.5 rpm at 120 fps: 0.43 bins a frame", 1.5, 120.0, 0, false, 900 },
	};
	for( const Raster& raster : kRasters )
		for( const Case& c : cases )
		{
			Rig rig;
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			quiet( rig );
			rig.plugin.SetCountForTest( true );
			rig.plugin.SetDecayForTest( false );
			rig.plugin.SetSweepAsLinesForTest( perturb.sweepLines );
			rig.Set( PT_RPM, ParamFromRpm( c.rpm ) );
			rig.Set( PT_DIRECTION, static_cast< float >( c.direction ) );
			rig.fps = c.fps;
			std::vector< double > times;
			double t = 0.0;
			for( int f = 0; f < c.frames; ++f )
			{
				times.push_back( t );
				t += c.jitter ? ( 0.008 + 0.022 * hash01( static_cast< uint32_t >( f ), 77 ) ) : 1.0 / c.fps;
			}
			if( !c.jitter )
				for( int f = 0; f < c.frames; ++f )
					times[ static_cast< size_t >( f ) ] = f / c.fps;
			rig.times = &times;

			//The rate the harness states, from the control's own mapping.
			const double rate = RpmFromParam( ParamFromRpm( c.rpm ) ) / 60.0 * kBearings * ( c.direction ? -1.0 : 1.0 );
			int wrongBins = 0, checkpoints = 0, zeros = 0, doubles = 0;
			double margin = 1.0;
			const int stops[] = { c.frames / 3, ( 2 * c.frames ) / 3, c.frames };
			int done = 0;
			for( int stop : stops )
			{
				if( !rig.Render( stop - done ) )
					return 1;
				done = stop;
				const double b = -0.5 + rate * ( times[ static_cast< size_t >( stop - 1 ) ] - times[ 0 ] );
				for( int f = 1; f < stop; ++f )
				{
					const double bf = -0.5 + rate * ( times[ static_cast< size_t >( f ) ] - times[ 0 ] );
					margin          = std::min( margin, std::fabs( bf - std::round( bf ) ) );
				}
				const Floats row = rig.PhosphorRect( 0, 0, kBearings, 1 );
				++checkpoints;
				const bool fullTurn = std::fabs( b + 0.5 ) >= kBearings;
				for( int j = 0; j < kBearings; ++j )
				{
					double expected;
					if( rate >= 0.0 )
						expected = b >= j ? std::floor( ( b - j ) / kBearings ) + 1.0 : 0.0;
					else
						expected = std::max( 0.0, std::floor( ( j - b ) / kBearings ) );
					const float got = row[ static_cast< size_t >( j ) * 2 ];
					if( got != static_cast< float >( expected ) )
						++wrongBins;
					if( fullTurn && got == 0.0f )
						++zeros;
					if( !fullTurn && got > 1.0f )
						++doubles;
				}
			}
			Check( wrongBins == 0 && margin > 1e-6,
			       fmt( "%dx%d  %s: %d of %d bin counts differ from the crossings the clock predicts over %d checkpoints "
			            "(%d unpainted after a full turn, %d painted twice within one; nearest frame to a bin centre %.1e bins)",
			            raster.w, raster.h, c.what, wrongBins, kBearings * checkpoints, checkpoints, zeros, doubles, margin ) );
		}
	return Verdict();
}

//===========================================================================
// --sweep-law (no GL): Sweep.h's crossing rule over a million random frames.
//===========================================================================
int runSweepLaw( const Perturb& perturb )
{
	std::printf( "\n=== sweep-law: the wedge rule paints each bin exactly once per rotation, any frame times (CPU)\n" );
	int bad = 0, runs = 0;
	for( int run = 0; run < 200; ++run )
	{
		std::vector< int > count( kBearings, 0 );
		const double rate = ( 0.5 + 120.0 * hash01( run, 1 ) ) / 60.0 * kBearings * ( hash01( run, 2 ) < 0.3 ? -1.0 : 1.0 );
		double b = -0.5 + 7.0 * hash01( run, 3 );
		const double start = b;
		for( int f = 0; f < 5000; ++f )
		{
			const double dt   = 0.001 + 0.05 * hash01( run, 100 + f );
			const double next = b + rate * dt;
			const sweep::Wedge w = perturb.lawLines ? sweep::LineAt( b, next, dt ) : sweep::Crossed( b, next, dt, kBearings );
			for( int i = 0; i < w.count; ++i )
				++count[ static_cast< size_t >( sweep::Column( w.first + i, kBearings ) ) ];
			b = next;
			if( std::fabs( b - start ) >= kBearings )
				break;
		}
		//Exactly the bins between start and b, each once (b crossed fewer than
		//a whole rotation's bins past the first turn only at the end).
		const double lo = std::min( start, b ), hi = std::max( start, b );
		int64_t crossed = 0;
		for( int j = 0; j < kBearings; ++j )
			crossed += count[ static_cast< size_t >( j ) ];
		const int64_t expected = rate > 0 ? sweep::floorToInt( hi ) - sweep::floorToInt( lo ) : sweep::ceilToInt( hi ) - sweep::ceilToInt( lo );
		int wrong = 0;
		for( int j = 0; j < kBearings; ++j )
			if( count[ static_cast< size_t >( j ) ] < 1 || count[ static_cast< size_t >( j ) ] > 2 )
				++wrong;
		if( crossed != expected || wrong > 0 )
			++bad;
		++runs;
	}
	Check( bad == 0, fmt( "%d random runs (0.5-120 rpm, either way, frames 1-51 ms apart, one turn each): %d with a gap, a "
	                      "double paint or a wrong total",
	                      runs, bad ) );
	return Verdict();
}

//===========================================================================
// --persist: the two-term decay, and the flash back when the beam returns.
//===========================================================================
int runPersist( const Perturb& perturb )
{
	std::printf( "\n=== persist: after the beam passes a target decays as A_f e^(-t/tau_f) + A_a e^(-t/tau_a), and is "
	             "repainted after 60/RPM s\n" );
	for( const Raster& raster : kRasters )
	{
		Rig rig;
		if( !rig.Init( raster.w, raster.h ) )
			return 1;
		quiet( rig );
		rig.plugin.SetDropFlashForTest( perturb.persistNoFlash );
		const double rpm = 20.0, tauA = 1.5, flash = 2.0;
		rig.Set( PT_RPM, ParamFromRpm( rpm ) );
		rig.Set( PT_PERSISTENCE, ParamFromPersistence( tauA ) );
		rig.Set( PT_FLASH, static_cast< float >( flash / 4.0 ) );
		rig.Set( PT_PHOSPHOR, static_cast< float >( Phosphor::P7 ) );
		rig.Set( PT_GAIN, ParamFromGainDb( -30.0 ) );
		rig.Set( PT_STC, 1.0f );
		rig.Set( PT_PULSE, ParamFromPulse( 2.0 ) );
		const double tauF   = FlashTauFor( Phosphor::P7 );
		const double aFlash = FlashFromParam( static_cast< float >( flash / 4.0 ) );
		const int column    = 512 + 97;//~102 degrees
		const double bearing = bearingOfBin( column );
		rig.plugin.SetTargetsForTest( { { static_cast< float >( bearing ), 0.5f, 1.0f } } );
		const int row = static_cast< int >( 0.5 * kRanges ) + 3;
		//The composite draws a window onto the polar grid, faded exactly as
		//the scope's pixels are; the target's texel is the window's (8, 8).
		rig.plugin.SetPolarViewForTest( true, column - 8, row - 8 );

		//The harness's own clock: when the antenna crossed the column.
		const double rate   = RpmFromParam( ParamFromRpm( rpm ) ) / 60.0 * kBearings;
		const double paint1 = ( column + 0.5 ) / rate;
		const double period = kBearings / rate;
		double energy       = -1.0;
		int frames = 0, wrongF = 0, wrongA = 0, n = 0;
		double worstF = 0.0, worstA = 0.0, repaintFlash = 0.0, firstFlash = 0.0;
		for( int f = 0; f < static_cast< int >( ( paint1 + 1.6 * period ) * 60.0 ); ++f )
		{
			if( !rig.Render( 1 ) )
				return 1;
			const double t = f / 60.0;
			if( t < paint1 )
				continue;
			Floats texel( 4 );
			glBindFramebuffer( GL_FRAMEBUFFER, rig.outputFBO );
			glReadPixels( 8, 8, 1, 1, GL_RGBA, GL_FLOAT, texel.data() );
			//Both paints so far, each decayed from its own crossing.
			double wantF = 0.0, wantA = 0.0;
			for( double p = paint1; p <= t; p += period )
			{
				wantF += aFlash * std::exp( -( t - p ) / tauF );
				wantA += std::exp( -( t - p ) / tauA );
			}
			if( energy < 0.0 )
			{
				//E is the target's echo, which the check does not know: read it
				//once, off the afterglow at the first frame, and predict the
				//rest from the law.
				energy     = texel[ 1 ] / wantA;
				firstFlash = texel[ 0 ];
			}
			++n;
			//Float: the fade is a double exp rounded once to float, times the
			//stored value (rounded once at its paint, twice by the second
			//paint's carry and add): a few ulp, whatever the frame count; 8 ulp
			//relative, and 1e-12 of E absolute where the flash has gone.
			const double tol = 8.0 * kUlp;
			const double eF  = std::fabs( texel[ 0 ] - energy * wantF ) - 1e-12 * energy;
			const double eA  = std::fabs( texel[ 1 ] - energy * wantA ) - 1e-12 * energy;
			worstF           = std::max( worstF, eF / ( energy * std::max( wantF, 1e-300 ) ) );
			worstA           = std::max( worstA, eA / ( energy * wantA ) );
			if( eF > tol * energy * wantF )
				++wrongF;
			if( eA > tol * energy * wantA )
				++wrongA;
			if( t >= paint1 + period && repaintFlash == 0.0 )
				repaintFlash = texel[ 0 ] / ( energy * aFlash * std::exp( -( t - paint1 - period ) / tauF ) );
			++frames;
		}
		Check( wrongF == 0 && wrongA == 0 && energy > 0.0 && firstFlash > 0.0,
		       fmt( "%dx%d  %d frames over 1.6 rotations: flash (tau %.3f s, x%.1f) off by at most %.2e relative, afterglow "
		            "(tau %.2f s) %.2e (bound 8 ulp)",
		            raster.w, raster.h, frames, tauF, aFlash, worstF, tauA, worstA ) );
		//The ratio of two such values: 16 ulp.
		Check( std::fabs( repaintFlash - 1.0 ) <= 16.0 * kUlp,
		       fmt( "%dx%d  60/RPM = %.3f s later the beam is back: the flash returns to %.6f of the first paint's level "
		            "(at the same age; bound 16 ulp)",
		            raster.w, raster.h, period, repaintFlash ) );
	}
	return Verdict();
}

//===========================================================================
// --r4: equal targets fall as R^-4 with STC off, and STC n flattens them.
//===========================================================================
int runR4( const Perturb& perturb )
{
	std::printf( "\n=== r4: equal targets' returns fall as R^(n-4): -4 with STC off, flat with STC at n = 4\n" );
	for( const Raster& raster : kRasters )
		for( double n : { 0.0, 2.0, 4.0 } )
		{
			Rig rig;
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			quiet( rig );
			rig.plugin.SetDecayForTest( false );
			rig.plugin.SetRangeExponentForTest( perturb.r4Dropped ? 0.0f : 4.0f );
			rig.Set( PT_RPM, ParamFromRpm( 120.0 ) );
			rig.Set( PT_GAIN, ParamFromGainDb( -30.0 ) );
			rig.Set( PT_STC, static_cast< float >( n / 4.0 ) );
			rig.Set( PT_RANGE, ParamFromRange( 12.0 ) );
			rig.Set( PT_PULSE, ParamFromPulse( 1.0 ) );
			//sigma 1e-4 at gain 1e-3: P = 1e-7 R^(n-4) <= 1e-3 at R = 0.1.
			const double ranges[] = { 0.1, 0.2, 0.4, 0.8 };
			const int columns[]   = { 170, 682, 1194, 1706 };//30, 120, 210, 300 degrees
			std::vector< world::Target > targets;
			for( int i = 0; i < 4; ++i )
				targets.push_back( { static_cast< float >( bearingOfBin( columns[ i ] ) ), static_cast< float >( ranges[ i ] ), 1e-4f } );
			rig.plugin.SetTargetsForTest( targets );
			if( !rig.Render( 31 ) )
				return 1;
			const Floats ph = rig.Phosphor();
			double x[ 4 ], y[ 4 ];
			for( int i = 0; i < 4; ++i )
			{
				double peak = 0.0;
				for( int r = 0; r < kRanges; ++r )
					peak = std::max( peak, static_cast< double >( at( ph, columns[ i ], r, 1 ) ) );
				x[ i ] = std::log( ranges[ i ] );
				y[ i ] = std::log( peak );
			}
			double mx = 0, my = 0;
			for( int i = 0; i < 4; ++i )
			{
				mx += x[ i ] / 4;
				my += y[ i ] / 4;
			}
			double sxy = 0, sxx = 0;
			for( int i = 0; i < 4; ++i )
			{
				sxy += ( x[ i ] - mx ) * ( y[ i ] - my );
				sxx += ( x[ i ] - mx ) * ( x[ i ] - mx );
			}
			const double slope = sxy / sxx;
			double worst       = 0.0;
			for( int i = 0; i < 4; ++i )
				worst = std::max( worst, std::fabs( y[ i ] - ( my + slope * ( x[ i ] - mx ) ) ) );
			//The video 1 - exp( -P ) against P: ln v = ln P - P/2 + ..., so a
			//point is off the line by at most P_max / 2 = 5e-4 in ln, which
			//over ln 8 of range moves the slope by at most 2.4e-4.
			const double tolSlope = 5e-4, tolPoint = 1e-3;
			Check( std::fabs( slope - ( n - 4.0 ) ) <= tolSlope && worst <= tolPoint,
			       fmt( "%dx%d  STC n = %.0f: slope of ln(return) on ln(R) %.6f (want %.0f, bound %.0e); worst point off the line "
			            "%.1e",
			            raster.w, raster.h, n, slope, n - 4.0, tolSlope, worst ) );
		}
	return Verdict();
}

//===========================================================================
// --over: a bright square in the clip is an echo where the polar mapping puts it.
//===========================================================================
int runOver( const Perturb& perturb )
{
	std::printf( "\n=== over: bright squares in the clip become echoes at the bearing and range the polar mapping predicts\n" );
	for( const Raster& raster : kRasters )
	{
		const int w = raster.w, h = raster.h;
		//The scope: inscribed (Scope Size 0), as the plugin computes it.
		const double radius = 0.95 * 0.5 * std::min( w, h ), cx = 0.5 * w, cy = 0.5 * h;
		struct Square
		{
			double bearingDeg, range;
			int x0, x1, y0, y1;///< pixel edges, exclusive
		};
		//North, east and south-west-ish on the axes: a square on an axis is
		//symmetric about its radial, so its echo's bearing is exact.
		std::vector< Square > squares = { { 0.0, 0.55, 0, 0, 0, 0 }, { 90.0, 0.35, 0, 0, 0, 0 }, { 180.0, 0.7, 0, 0, 0, 0 } };
		const double half = 0.08 * radius;
		Floats card( static_cast< size_t >( w ) * h * 4, 0.0f );
		for( size_t i = 0; i < card.size(); i += 4 )
			card[ i + 3 ] = 1.0f;
		for( Square& s : squares )
		{
			const double b  = s.bearingDeg * kPi / 180.0;
			const double sx = cx + s.range * radius * std::sin( b ), sy = cy + s.range * radius * std::cos( b );
			s.x0            = static_cast< int >( std::lround( sx - half ) );
			s.x1            = static_cast< int >( std::lround( sx + half ) );
			s.y0            = static_cast< int >( std::lround( sy - half ) );
			s.y1            = static_cast< int >( std::lround( sy + half ) );
			//Keep the square symmetric about its axis exactly.
			if( s.bearingDeg == 0.0 || s.bearingDeg == 180.0 )
			{
				const int hw = static_cast< int >( std::lround( half ) );
				s.x0         = static_cast< int >( std::lround( cx ) ) - hw;
				s.x1         = static_cast< int >( std::lround( cx ) ) + hw;
			}
			else
			{
				const int hw = static_cast< int >( std::lround( half ) );
				s.y0         = static_cast< int >( std::lround( cy ) ) - hw;
				s.y1         = static_cast< int >( std::lround( cy ) ) + hw;
			}
			for( int y = s.y0; y < s.y1; ++y )
				for( int x = s.x0; x < s.x1; ++x )
					for( int c = 0; c < 3; ++c )
						card[ ( static_cast< size_t >( y ) * w + x ) * 4 + c ] = 1.0f;
		}

		Rig rig( true );
		if( !rig.Init( w, h, &card ) )
			return 1;
		quiet( rig );
		rig.plugin.SetDecayForTest( false );
		rig.plugin.SetMirrorForTest( perturb.overMirrored );
		rig.Set( PT_SCOPE_SIZE, 0.0f );
		rig.Set( PT_THRESHOLD, 0.0f );
		rig.Set( PT_RPM, ParamFromRpm( 120.0 ) );
		rig.Set( PT_BEAMWIDTH, ParamFromBeamwidth( 1.0 ) );
		rig.Set( PT_SIDELOBES, 0.0f );
		rig.Set( PT_GAIN, ParamFromGainDb( -30.0 ) );
		rig.Set( PT_STC, 1.0f );
		const double rangeKm = 12.0, pulse = 1.0;
		rig.Set( PT_RANGE, ParamFromRange( rangeKm ) );
		rig.Set( PT_PULSE, ParamFromPulse( pulse ) );
		rig.Set( PT_MIX, 1.0f );
		if( !rig.Render( 31 ) )
			return 1;
		const Floats ph   = rig.Phosphor();
		const double L    = kMetresPerMicrosecond * pulse / 1000.0 / rangeKm;
		const double binDeg = 360.0 / kBearings;
		//Half a clip pixel (the edge is reconstructed bilinearly: its half level
		//is the pixel boundary, and the pulse's midpoint quadrature can move it
		//by at most half a pixel's ramp) plus half a range bin (the profile is
		//read at bin centres), in scope units.
		const double tolRange = 0.5 / radius + 0.5 / kRanges;
		for( const Square& s : squares )
		{
			const double expectBearing = s.bearingDeg;
			//The two columns either side of the square's bearing (a bin edge on
			//every axis), averaged.
			const int kRight = sweep::Column( static_cast< int64_t >( std::lround( expectBearing / binDeg ) ), kBearings );
			const int kLeft  = sweep::Column( kRight - 1, kBearings );
			std::vector< double > profile;
			for( int i = 0; i < kRanges; ++i )
				profile.push_back( 0.5 * ( at( ph, kLeft, i, 1 ) + at( ph, kRight, i, 1 ) ) );
			const Width wd = halfMaxWidth( profile );
			//Where the square's edges are along its radial, from its PIXELS.
			double nearEdge, farEdge;
			if( s.bearingDeg == 0.0 )
			{
				nearEdge = ( s.y0 - cy ) / radius;
				farEdge  = ( s.y1 - cy ) / radius;
			}
			else if( s.bearingDeg == 90.0 )
			{
				nearEdge = ( s.x0 - cx ) / radius;
				farEdge  = ( s.x1 - cx ) / radius;
			}
			else
			{
				nearEdge = ( cy - s.y1 ) / radius;
				farEdge  = ( cy - s.y0 ) / radius;
			}
			const double gotNear = wd.valid ? ( wd.left + 0.5 ) / kRanges : -1.0;
			const double gotFar  = wd.valid ? ( wd.right + 0.5 ) / kRanges : -1.0;
			const bool rangeOk   = wd.valid && std::fabs( gotNear - ( nearEdge + 0.5 * L ) ) <= tolRange
			                     && std::fabs( gotFar - ( farEdge + 0.5 * L ) ) <= tolRange;

			//Bearing: the echo's power-weighted centroid in bearing over the
			//square's rows, within +-20 degrees of where it should be.
			double sw = 0.0, sb = 0.0;
			const int rowLo = static_cast< int >( ( nearEdge + L ) * kRanges ) + 1, rowHi = static_cast< int >( farEdge * kRanges ) - 1;
			const int span  = static_cast< int >( 20.0 / binDeg );
			for( int i = rowLo; i <= rowHi; ++i )
				for( int d = -span; d < span; ++d )
				{
					const int k    = sweep::Column( kRight + d, kBearings );
					const double v = at( ph, k, i, 1 );
					sw += v;
					sb += v * ( d + 0.5 );
				}
			const double centroidDeg = sw > 0.0 ? ( sb / sw ) * binDeg : 999.0;
			//The pixel grid is symmetric about the square's axis to the pixel
			//(its edges were placed on whole pixels either side of the centre
			//line), and the polar samples are mirror pairs about it, so the
			//centroid is exact up to the float sine/cosine of +-theta: 0.01
			//bin covers that many times over.
			const bool bearingOk = sw > 0.0 && std::fabs( centroidDeg ) <= 0.01 * binDeg;
			Check( rangeOk && bearingOk,
			       fmt( "%dx%d  square at %.0f deg, %.2f: echo's centroid %+.2e deg off the bearing (bound %.4f); half-power edges "
			            "at %.4f..%.4f, predicted %.4f..%.4f (edges + L/2, bound %.4f)",
			            w, h, s.bearingDeg, s.range, centroidDeg, 0.01 * binDeg, gotNear, gotFar, nearEdge + 0.5 * L,
			            farEdge + 0.5 * L, tolRange ) );
		}

		//The alpha, and the clip untouched at Mix 0.
		{
			const Floats out = rig.Output();
			float minAlpha   = 1.0f;
			for( size_t i = 3; i < out.size(); i += 4 )
				minAlpha = std::min( minAlpha, out[ i ] );
			Floats clear = card;
			for( size_t i = 3; i < clear.size(); i += 4 )
				clear[ i ] = ( i / 4 ) % 7 == 0 ? 0.0f : 0.6f;
			rig.Upload( clear );
			rig.Set( PT_MIX, 0.0f );
			rig.Render( 1 );
			const Floats same = rig.Output();
			int differ        = 0;
			for( size_t i = 0; i < same.size(); ++i )
				differ += same[ i ] != clear[ i ];
			Check( minAlpha == 1.0f && differ == 0,
			       fmt( "%dx%d  Mix 1 is opaque everywhere (least alpha %.3f); Mix 0 returns a clip with alpha bit-exact (%d floats "
			            "differ)",
			            w, h, minAlpha, differ ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --prime: no audio event on the first frame after a clip trigger.
//===========================================================================
int runPrime( const Perturb& perturb )
{
	std::printf( "\n=== prime: loud audio already playing when the clip is triggered fires nothing on the trigger frame\n" );
	for( bool effect : { false, true } )
		for( const Raster& raster : kRasters )
		{
			Rig rig( effect );
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			quiet( rig );
			rig.Set( PT_THRESHOLD, 1.0f );//the Over's clip reflects nothing: the analyser is the subject
			rig.plugin.SetUnprimedForTest( perturb.primeOff );
			rig.Set( PT_AUDIO_STROBE, 0.8f );
			rig.feed = AudioFeed::Pulses;
			rig.Render( 100 );//1.67 s of beats
			const unsigned long long before = rig.plugin.EventsFired();
			//The trigger: the host's clock goes back to 0.02 s -- just after a
			//hit, the music still loud, which is the case that deafened the
			//fleet's unprimed analysers. The next hit is at 0.5 s, 29 frames on.
			rig.clockOffset = 0.02 - static_cast< double >( rig.frame ) / rig.fps;
			rig.Render( 1 );
			const unsigned long long atTrigger = rig.plugin.EventsFired();
			rig.Render( 35 );
			const unsigned long long after = rig.plugin.EventsFired();
			Check( before >= 3 && atTrigger == before && after == atTrigger + 1,
			       fmt( "%s %dx%d: %llu events from 4 beats before; %llu on the trigger frame; %llu in the next 35 frames (one "
			            "hit, at 0.5 s)",
			            effect ? "Over  " : "source", raster.w, raster.h, before, atTrigger - before, after - atTrigger ) );
		}
	return Verdict();
}

//===========================================================================
// --resize: the phosphor survives the host's raster changing.
//===========================================================================
int runResize( const Perturb& perturb )
{
	std::printf( "\n=== resize: the phosphor survives a resize mid-run, to the last bit\n" );
	//Between the two rasters both ways; with one raster given, between it and
	//its half.
	std::vector< std::pair< Raster, Raster > > pairs;
	if( kRasters.size() >= 2 )
		pairs = { { kRasters[ 0 ], kRasters[ 1 ] }, { kRasters[ 1 ], kRasters[ 0 ] } };
	else
		pairs = { { kRasters[ 0 ], { kRasters[ 0 ].w / 2, kRasters[ 0 ].h / 2 } } };
	for( bool effect : { false, true } )
		for( const auto& pair : pairs )
		{
			const Raster a = pair.first, b = pair.second;
			Rig rig( effect );
			if( !rig.Init( a.w, a.h ) )
				return 1;
			rig.plugin.SetClearOnResizeForTest( perturb.resizeClears );
			//Clutter, noise and traffic, no land: plenty lit, and cheap on the
			//software renderer.
			rig.Set( PT_LAND, 0.0f );
			rig.Set( PT_THRESHOLD, 1.0f );
			rig.Set( PT_CONTACTS, 16.0f );
			if( !rig.Render( 150 ) )
				return 1;
			//From here the antenna is stopped, so nothing is painted: the state
			//must not change at all, and the picture only fade.
			rig.Set( PT_RPM, 0.0f );
			rig.plugin.SetPolarViewForTest( true, 400, 200 );
			if( !rig.Render( 1 ) )
				return 1;
			const Floats before = rig.Phosphor();
			const Floats viewBefore = rig.Output();
			const int wa = rig.width, ha = rig.height;
			if( !rig.Resize( b.w, b.h ) || !rig.Render( 1 ) )
				return 1;
			const Floats after = rig.Phosphor();
			const Floats viewAfter = rig.Output();
			const double tauF  = FlashTauFor( Phosphor::P7 );
			const double tauA  = PersistenceFromParam( rig.plugin.GetById( PT_PERSISTENCE ) );
			const double keepF = std::exp( -( 1.0 / 60.0 ) / tauF ), keepA = std::exp( -( 1.0 / 60.0 ) / tauA );
			int lit = 0, wrong = 0, faded = 0;
			for( size_t i = 0; i < before.size(); i += 2 )
			{
				lit += before[ i + 1 ] > 1e-3f;
				wrong += before[ i ] != after[ i ] || before[ i + 1 ] != after[ i + 1 ];
			}
			//The picture over the window both rasters share: one frame's fade.
			//Each side is a stored value times a double exp rounded to float,
			//so the ratio is right to 3 ulp.
			for( int y = 0; y < std::min( ha, rig.height ); ++y )
				for( int x = 0; x < std::min( wa, rig.width ); ++x )
				{
					const float* p0 = &viewBefore[ ( static_cast< size_t >( y ) * wa + x ) * 4 ];
					const float* p1 = &viewAfter[ ( static_cast< size_t >( y ) * rig.width + x ) * 4 ];
					if( std::fabs( p1[ 0 ] - p0[ 0 ] * keepF ) > 3.0 * kUlp * p0[ 0 ] * keepF
					    || std::fabs( p1[ 1 ] - p0[ 1 ] * keepA ) > 3.0 * kUlp * p0[ 1 ] * keepA )
						++faded;
				}
			Check( lit > 5000 && wrong == 0 && faded == 0,
			       fmt( "%s %dx%d -> %dx%d after 150 frames: %d texels lit, %d changed (bound: none); %d picture texels not "
			            "their previous value x one frame's fade (bound 3 ulp)",
			            effect ? "Over  " : "source", a.w, a.h, b.w, b.h, lit, wrong, faded ) );
		}
	return Verdict();
}

//===========================================================================
// --clock: at Resolume's ~499 million ms, the antenna still turns by real
// elapsed time to the millionth of a bin.
//===========================================================================
int runClock( const Perturb& perturb )
{
	std::printf( "\n=== clock: a host clock at 499,000,000 ms (a float resolves 32 ms there) turns the antenna exactly\n" );
	for( const Raster& raster : kRasters )
	{
		Rig rig;
		if( !rig.Init( raster.w, raster.h, nullptr, false ) )
			return 1;
		quiet( rig );
		rig.plugin.SetFloatClockForTest( perturb.clockFloat );
		rig.plugin.SetCountForTest( true );
		rig.plugin.SetDecayForTest( false );
		rig.hostUnit    = 1000.0;//milliseconds
		rig.clockOffset = 499000.0;
		//The unit vote needs the wall clock to agree: the first frames are
		//paced in real time, as a host would pace them.
		int paced = 0;
		while( rig.plugin.ClockScale() == 0.0 && paced < 60 )
		{
			rig.Render( 1 );
			std::this_thread::sleep_for( std::chrono::microseconds( 16667 ) );
			++paced;
		}
		rig.Render( 2 );
		const bool voted = rig.plugin.ClockScale() == 0.001;
		const double rate = RpmFromParam( rig.plugin.GetById( PT_RPM ) ) / 60.0 * kBearings;
		const int m       = rig.frame - 1;
		const double bm   = rig.plugin.AntennaBins();
		double worstAdvance = 0.0, worstDt = 0.0;
		int wrongTotals = 0;
		for( int stop = 0; stop < 10; ++stop )
		{
			for( int f = 0; f < 60; ++f )
			{
				rig.Render( 1 );
				worstDt = std::max( worstDt, std::fabs( rig.plugin.LastDt() - 1.0 / 60.0 ) );
			}
			const int n = rig.frame - 1;
			//The harness's own arithmetic, in double, from the times it sent.
			const double predicted = bm + rate * ( rig.TimeOf( n ) - rig.TimeOf( m ) );
			worstAdvance = std::max( worstAdvance, std::fabs( rig.plugin.AntennaBins() - predicted ) );
			//And the picture: every crossing the antenna made is a paint.
			const Floats row = rig.PhosphorRect( 0, 0, kBearings, 1 );
			double total     = 0.0;
			for( int j = 0; j < kBearings; ++j )
				total += row[ static_cast< size_t >( j ) * 2 ];
			if( total != std::floor( predicted ) + 1.0 )
				++wrongTotals;
		}
		//Double at 4.99e8 ms: an ulp is 6e-8 ms, so a frame's dt is right to
		//1e-10 s and 600 frames of it to 6e-8 bins at 819 bins/s.
		Check( voted && worstAdvance <= 1e-6 && worstDt <= 1e-9 && wrongTotals == 0,
		       fmt( "%dx%d  the vote settled on ms (%s, after %d paced frames); over 600 frames every dt within %.1e s of 1/60, "
		            "the antenna within %.1e bins of rate x elapsed (bound 1e-6), paints = crossings at %d of 10 checkpoints",
		            raster.w, raster.h, voted ? "yes" : "NO", paced, worstDt, worstAdvance, 10 - wrongTotals ) );
	}
	return Verdict();
}

//===========================================================================
// --cues (no GL): options, booleans, events and integers step; the rest ramp.
//===========================================================================
int runCues( const Perturb& perturb )
{
	std::printf( "\n=== cues: a cue sheet steps options, booleans, events and integers, and ramps standard controls\n" );
	std::istringstream sheet( "0 Phosphor 0\n60 Phosphor 2\n0 Heading Line 0\n30 Heading Line 1\n"
	                          "0 Contacts 2\n60 Contacts 12\n0 Audio Strobe 0\n60 Audio Strobe 1\n" );
	std::string error;
	const auto tracks = loadScript( sheet, "cues", error );
	RadarPlugin plugin( false );
	int bad = 0;
	std::string what;
	for( const NamedParameter& p : listParameters( plugin ) )
	{
		const auto found = tracks.find( p.name );
		if( found == tracks.end() )
			continue;
		const bool ramp = perturb.cuesRamp ? true : !stepsBetweenCues( p.type );
		const float mid = valueAt( found->second, 29, ramp );
		const float a = found->second.front().second, b = found->second.back().second;
		const int bFrame = found->second.back().first;
		const float atB  = valueAt( found->second, bFrame, ramp );
		const bool expectRamp = p.type == FF_TYPE_STANDARD;
		const bool ok         = expectRamp ? ( mid > a && mid < b && atB == b ) : ( mid == a && valueAt( found->second, bFrame - 1, ramp ) == a && atB == b );
		bad += !ok;
		what += fmt( " %s %s(%g at frame 29)", p.name.c_str(), expectRamp ? "ramps " : "steps ", mid );
	}
	Check( error.empty() && bad == 0, fmt( "%d wrong:%s", bad, what.c_str() ) );
	return Verdict();
}

//===========================================================================
// --names (no GL)
//===========================================================================
int runNames( const Perturb& )
{
	std::printf( "\n=== names: every parameter unique (as Arena addresses them, too) and within FFGL's 16 characters\n" );
	for( bool effect : { false, true } )
	{
		RadarPlugin plugin( effect );
		std::map< std::string, int > seen, address;
		int longNames = 0, dupes = 0, clashes = 0;
		for( unsigned int i = 0; i < plugin.ParamCount(); ++i )
		{
			const std::string name = plugin.GetParamName( i ) ? plugin.GetParamName( i ) : "";
			if( name.size() > 16 )
			{
				std::printf( "    too long: %s\n", name.c_str() );
				++longNames;
			}
			if( seen[ name ]++ > 0 )
				++dupes;
			//Arena's OSC/REST address: lower case, spaces removed.
			std::string key;
			for( char c : name )
				if( c != ' ' )
					key += static_cast< char >( std::tolower( static_cast< unsigned char >( c ) ) );
			if( address[ key ]++ > 0 )
				++clashes;
		}
		//The About block is last, so a "User guide" button added later moves
		//no control's index.
		const unsigned int aboutFirst = plugin.ParamCount() - stoatworks::about::kParamCount;
		const bool aboutLast = std::string( plugin.GetParamName( aboutFirst ) ) == "About";
		Check( longNames == 0 && dupes == 0 && clashes == 0 && aboutLast,
		       fmt( "%s: %u parameters, %d too long, %d duplicated, %d clashing addresses; the About block is last (%s)",
		            effect ? "SW Radar Over" : "SW Radar", plugin.ParamCount(), longNames, dupes, clashes, aboutLast ? "yes" : "NO" ) );
	}
	return Verdict();
}

//===========================================================================
// --state: the GL state the host hands over is the state it gets back.
//===========================================================================
int runState( const Perturb& )
{
	std::printf( "\n=== state: the GL state the host hands over is the state it gets back\n" );
	for( bool effect : { false, true } )
	{
		Rig rig( effect );
		if( !rig.Init( 320, 180 ) )
			return 1;
		GLuint hostArray = 0, hostBuffer = 0;
		glGenVertexArrays( 1, &hostArray );
		glGenBuffers( 1, &hostBuffer );
		int problems = 0;
		std::string what;
		for( int frame = 0; frame < 3; ++frame )
		{
			glBindFramebuffer( GL_FRAMEBUFFER, rig.outputFBO );
			glViewport( 7, 5, 300, 170 );
			glBindVertexArray( hostArray );
			glBindBuffer( GL_ARRAY_BUFFER, hostBuffer );
			glEnable( GL_BLEND );
			glBlendFuncSeparate( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO );
			glClearColor( 0.2f, 0.3f, 0.4f, 0.5f );
			glEnable( GL_SCISSOR_TEST );
			glScissor( 0, 0, 320, 180 );
			glActiveTexture( GL_TEXTURE0 );
			glUseProgram( 0 );
			rig.plugin.SetTime( frame / 60.0 );
			if( rig.plugin.ProcessOpenGL( &rig.process ) != FF_SUCCESS )
				return 1;
			GLint viewport[ 4 ] = {}, array = 0, buffer = 0, program = 0, unit = 0, fbo = 0, src = 0, dst = 0;
			GLfloat clear[ 4 ] = {};
			glGetIntegerv( GL_VIEWPORT, viewport );
			glGetIntegerv( GL_VERTEX_ARRAY_BINDING, &array );
			glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &buffer );
			glGetIntegerv( GL_CURRENT_PROGRAM, &program );
			glGetIntegerv( GL_ACTIVE_TEXTURE, &unit );
			glGetIntegerv( GL_FRAMEBUFFER_BINDING, &fbo );
			glGetIntegerv( GL_BLEND_SRC_RGB, &src );
			glGetIntegerv( GL_BLEND_DST_RGB, &dst );
			glGetFloatv( GL_COLOR_CLEAR_VALUE, clear );
			auto expect = [ & ]( bool ok, const char* name ) {
				if( !ok )
				{
					++problems;
					what += std::string( " " ) + name;
				}
			};
			expect( viewport[ 0 ] == 7 && viewport[ 1 ] == 5 && viewport[ 2 ] == 300 && viewport[ 3 ] == 170, "viewport" );
			expect( array == static_cast< GLint >( hostArray ), "vertex-array" );
			expect( buffer == static_cast< GLint >( hostBuffer ), "array-buffer" );
			expect( program == 0, "program" );
			expect( unit == GL_TEXTURE0, "active-unit" );
			expect( fbo == static_cast< GLint >( rig.outputFBO ), "framebuffer" );
			expect( glIsEnabled( GL_BLEND ) && src == GL_SRC_ALPHA && dst == GL_ONE_MINUS_SRC_ALPHA, "blend" );
			expect( glIsEnabled( GL_SCISSOR_TEST ), "scissor" );
			expect( clear[ 0 ] == 0.2f && clear[ 1 ] == 0.3f && clear[ 2 ] == 0.4f && clear[ 3 ] == 0.5f, "clear-colour" );
			for( int u = 0; u < 10; ++u )
			{
				GLint bound = 0;
				glActiveTexture( static_cast< GLenum >( GL_TEXTURE0 + u ) );
				glGetIntegerv( GL_TEXTURE_BINDING_2D, &bound );
				expect( bound == 0, "texture-unit" );
			}
			glActiveTexture( GL_TEXTURE0 );
		}
		glDisable( GL_SCISSOR_TEST );
		glDisable( GL_BLEND );
		glBindVertexArray( 0 );
		glBindBuffer( GL_ARRAY_BUFFER, 0 );
		glDeleteVertexArrays( 1, &hostArray );
		glDeleteBuffers( 1, &hostBuffer );
		Check( problems == 0, fmt( "%s, three frames: viewport, vertex array, array buffer, program, active unit, framebuffer, "
		                           "blend, scissor, clear colour, ten texture units (%d wrong:%s)",
		                           effect ? "Over" : "source", problems, what.empty() ? " none" : what.c_str() ) );
	}
	return Verdict();
}

struct CheckEntry
{
	const char* flag;
	CheckFn run;
	bool offline;///< needs no GL context
};

const std::vector< CheckEntry >& checks()
{
	static const std::vector< CheckEntry > list = {
		{ "arc", runArc, false },       { "pulse", runPulse, false },   { "sweep", runSweep, false },
		{ "sweep-law", runSweepLaw, true }, { "persist", runPersist, false }, { "r4", runR4, false },
		{ "over", runOver, false },     { "prime", runPrime, false },   { "resize", runResize, false },
		{ "clock", runClock, false },   { "cues", runCues, true },      { "names", runNames, true },
		{ "state", runState, false },
	};
	return list;
}

bool isOffline( const std::string& flag )
{
	for( const CheckEntry& c : checks() )
		if( flag == c.flag )
			return c.offline;
	return false;
}

//===========================================================================
// --negative
//===========================================================================
int runNegative( bool offlineOnly = false )
{
	struct Case
	{
		const char* name;
		CheckFn check;
		Perturb perturb;
		const char* what;
	};
	std::vector< Case > cases;
	auto add = [ & ]( const char* name, CheckFn fn, const char* what, std::function< void( Perturb& ) > set ) {
		Perturb p;
		set( p );
		cases.push_back( { name, fn, p, what } );
	};
	add( "arc", runArc, "paint the one-way pattern (sqrt 2 wider)", []( Perturb& p ) { p.arcOneWay = true; } );
	add( "pulse", runPulse, "a pulse extent of c tau, not c tau / 2", []( Perturb& p ) { p.pulseRoundTrip = true; } );
	add( "sweep", runSweep, "sample the sweep as one line per frame", []( Perturb& p ) { p.sweepLines = true; } );
	add( "sweep-law", runSweepLaw, "Sweep.h's LineAt in place of Crossed", []( Perturb& p ) { p.lawLines = true; } );
	add( "persist", runPersist, "paint no flash: one term, not two", []( Perturb& p ) { p.persistNoFlash = true; } );
	add( "r4", runR4, "drop the R^4 from the radar equation", []( Perturb& p ) { p.r4Dropped = true; } );
	add( "over", runOver, "lay the clip under the scope mirrored (bearings anticlockwise)", []( Perturb& p ) { p.overMirrored = true; } );
	add( "prime", runPrime, "run the analyser unprimed", []( Perturb& p ) { p.primeOff = true; } );
	add( "resize", runResize, "clear the phosphor on a resize", []( Perturb& p ) { p.resizeClears = true; } );
	add( "clock", runClock, "take elapsed time from the host clock in float", []( Perturb& p ) { p.clockFloat = true; } );
	add( "cues", runCues, "ramp every control between keys", []( Perturb& p ) { p.cuesRamp = true; } );

	if( offlineOnly )
		cases.erase( std::remove_if( cases.begin(), cases.end(), []( const Case& c ) { return !isOffline( c.name ); } ), cases.end() );

	int unfalsifiable = 0;
	for( const Case& c : cases )
	{
		std::printf( "\n=== negative control: %s -- %s\n", c.name, c.what );
		const int before = g_failures;
		g_failures       = 0;
		c.check( c.perturb );
		const int observed = g_failures;
		g_failures         = before;
		if( observed > 0 )
			std::printf( "  ok    %s failed %d check%s, as it must\n", c.name, observed, observed == 1 ? "" : "s" );
		else
		{
			std::printf( "  FAIL  %s PASSED against a wrong model -- it cannot fail, so it is not a check\n", c.name );
			++unfalsifiable;
		}
	}
	std::printf( "\nnegative controls: %zu wrong models, %d of them undetected\n", cases.size(), unfalsifiable );
	std::printf( "\n  %s\n", unfalsifiable == 0 ? "PASS" : "FAIL" );
	return unfalsifiable == 0 ? 0 : 1;
}
} // namespace

//---------------------------------------------------------------------------
int main( int argc, char** argv )
{
	std::string outPath = "/tmp/radar.png";
	std::vector< std::string > settings;
	int width = 1280, height = 720, frames = 150;
	double fps = 60.0;
	bool beat = false, effect = false;
	std::string mode, scriptPath, clipPath;
	int filmFrames = -1;
	bool sizeGiven = false, framesGiven = false;

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" || argument == "-h" )
		{
			std::printf( "ratest -- render Radar offline and measure its scope\n\n"
			             "  --out PATH        render and write a PNG (default /tmp/radar.png)\n"
			             "  --over            the Over effect, on the harness's harbour card\n"
			             "  --clip FILE       (with --over --out) a raw RGBA frame of --size to use as the clip\n"
			             "  --size WxH        render size (default 1280x720)\n"
			             "  --frames N        frames before reading back (default 150: one rotation at 24 rpm)\n"
			             "  --fps N           the synthetic clock's rate (default 60)\n"
			             "  --beat            feed a beat every half second into the Audio buffer\n"
			             "  --set \"Name=V\"    set a parameter by its display name. Repeatable.\n"
			             "  --list            every parameter and its default\n"
			             "  --pipe            raw RGBA out: the source makes --frames N (0/absent: until the\n"
			             "                    reader hangs up); the Over effect takes frames in on stdin\n"
			             "  --film N          N frames, raw RGBA on stdout (the Over on its card)\n"
			             "  --script PATH     cues for --pipe/--film: 'frame Name value'\n\n"
			             "  checks: --arc --pulse --sweep --persist --r4 --over-check --prime --resize --clock --state\n"
			             "          --sweep-law --cues --names (no GL)   --negative   --bench\n"
			             "  --offline         the checks and negative controls that need no GL context (CI)\n" );
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
		{
			frames      = std::atoi( argv[ ++i ] );
			framesGiven = true;
		}
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--beat" )
			beat = true;
		else if( argument == "--over" )
			effect = true;
		else if( argument == "--clip" && hasNext )
			clipPath = argv[ ++i ];
		else if( argument == "--pipe" )
			mode = "pipe";
		else if( argument == "--film" && hasNext )
		{
			mode       = "film";
			filmFrames = std::max( 1, std::atoi( argv[ ++i ] ) );
		}
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--list" )
			mode = "list";
		else if( argument == "--size" && hasNext )
		{
			const std::string value = argv[ ++i ];
			const size_t cross      = value.find( 'x' );
			if( cross != std::string::npos )
			{
				width  = std::atoi( value.substr( 0, cross ).c_str() );
				height = std::atoi( value.substr( cross + 1 ).c_str() );
			}
			sizeGiven = true;
		}
		else if( argument == "--over-check" )
			mode = "over";
		else if( argument.rfind( "--", 0 ) == 0 )
			mode = argument.substr( 2 );
		else
		{
			std::fprintf( stderr, "unknown argument '%s' (try --help)\n", argument.c_str() );
			return 2;
		}
	}
	if( width <= 0 || height <= 0 || !( fps > 0.0 ) )
	{
		std::fprintf( stderr, "--size and --fps must be positive\n" );
		return 2;
	}
	//A check given --size runs at that raster alone (the software pass).
	if( sizeGiven )
		kRasters = { { width, height } };

	if( mode == "expect" )
	{
		//A DRAFT of the fleet gate's expectation (plugin-bench/arena/expect/
		//radar.json), from what the plugins really declare: defaults are the
		//floats the constructors set, not rounded literals (containment's
		//trap). Where a control needs a context to show, it says so.
		std::printf( "{\n  \"plugin\": \"radar\",\n  \"dlls\": [\"Radar.dll\", \"Radar Over.dll\"],\n"
		             "  \"register\": [\n    {\"name\": \"SW Radar\", \"uid\": \"RA01\", \"kind\": \"source\"},\n"
		             "    {\"name\": \"SW Radar Over\", \"uid\": \"RA02\", \"kind\": \"effect\"}\n  ],\n  \"params\": {\n" );
		for( bool over : { false, true } )
		{
			RadarPlugin plugin( over );
			std::printf( "    \"%s\": [\n", over ? "SW Radar Over" : "SW Radar" );
			const std::vector< NamedParameter > list = listParameters( plugin );
			for( size_t i = 0; i < list.size(); ++i )
			{
				const NamedParameter& p = list[ i ];
				std::string line = "      {\"name\": \"" + p.name + "\", ";
				if( p.type == FF_TYPE_OPTION )
					line += "\"type\": \"ParamChoice\", \"default\": \"" + std::string( plugin.GetParamElementName( p.index, static_cast< unsigned int >( std::lround( p.value ) ) ) ) + "\"";
				else if( p.type == FF_TYPE_BUFFER )
					line += "\"type\": \"ParamChoice\", \"default\": \"Composition\", \"requires\": \"audio\"";
				else if( p.type == FF_TYPE_BOOLEAN )
					line += std::string( "\"type\": \"ParamBoolean\", \"default\": " ) + ( p.value > 0.5f ? "true" : "false" );
				else if( p.type == FF_TYPE_EVENT )
					line += "\"type\": \"ParamEvent\"";
				else if( p.type == FF_TYPE_TEXT )
					line += "\"type\": \"ParamString\", \"default_pattern\": \"^Radar\\\\ v{version}\\\\ \\\\-\\\\ MIT\\\\ \\\\-\\\\ Stoatworks\\\\ Labs,\\\\ stoatworks\\\\-labs\\\\.com$\"";
				else
				{
					const float lo = p.type == FF_TYPE_INTEGER ? plugin.GetParamRange( p.index ).min : 0.0f;
					const float hi = p.type == FF_TYPE_INTEGER ? plugin.GetParamRange( p.index ).max : 1.0f;
					line += fmt( "\"type\": \"ParamRange\", \"min\": %.1f, \"max\": %.1f, \"default\": %.17g", lo, hi, static_cast< double >( p.value ) );
				}
				if( p.name.rfind( "Audio ", 0 ) == 0 )
					line += ", \"requires\": \"audio\"";
				if( over && ( p.name == "Bearing Marks" || p.name == "Heading Line" ) )
					line += ", \"needs\": {\"Scope Size\": 0.0}";
				if( p.name == "Direction" || p.name == "RPM" || p.name == "Persistence" || p.name == "Flash" )
					line += ", \"note\": \"acts over time: a still carrier and single grabs may read it DEAD (the gate's blind spot)\"";
				line += i + 1 < list.size() ? "},\n" : "}\n";
				std::printf( "%s", line.c_str() );
			}
			std::printf( over ? "    ]\n" : "    ],\n" );
		}
		std::printf( "  }\n}\n" );
		return 0;
	}
	if( mode == "list" )
	{
		RadarPlugin plugin( effect );
		std::printf( "%-3s %-18s %-9s %s\n", "id", "name", "kind", "default" );
		for( const NamedParameter& parameter : listParameters( plugin ) )
			std::printf( "%-3u %-18s %-9s %.4f\n", parameter.index, parameter.name.c_str(), kindName( parameter.type ), parameter.value );
		return 0;
	}

	//A reader that hangs up must end --pipe/--film with exit 1 and a message,
	//not SIGPIPE's silent 141: ignored here, the write fails with EPIPE.
	std::signal( SIGPIPE, SIG_IGN );

	if( mode == "offline" )
	{
		//No context at all: what a runner with no accelerated GL can run. The
		//skip is loud, so a green run is not read as one that checked pixels.
		int failed = 0;
		for( const CheckEntry& check : checks() )
			if( check.offline )
				failed |= check.run( Perturb {} );
		failed |= runNegative( true );
		std::printf( "\n  offline: the checks that need no GL context. The shader and pixel checks were NOT run --\n"
		             "  tools/verify.sh runs them against a real driver, at 320x180 and above, and again on the software renderer.\n"
		             "\n  %s\n", failed == 0 ? "PASS" : "FAIL" );
		return failed == 0 ? 0 : 1;
	}
	for( const CheckEntry& check : checks() )
		if( mode == check.flag && check.offline )
			return check.run( Perturb {} );

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL 4.1 core context\n" );
		return 1;
	}

	int result = 0;
	bool ran   = false;
	for( const CheckEntry& check : checks() )
		if( mode == check.flag )
		{
			result = check.run( Perturb {} );
			ran    = true;
		}

	if( ran )
		;
	else if( mode == "pipe" )
		//The fleet's two shapes: an effect is frames in, frames out (toner's);
		//a source makes --frames of them, or runs until the reader hangs up
		//(pattern's).
		result = runPipe( effect, width, height, fps, scriptPath, framesGiven ? frames : 0, effect, beat, settings );
	else if( mode == "film" )
		result = runPipe( effect, width, height, fps, scriptPath, filmFrames, false, beat, settings );
	else if( mode == "negative" )
		result = runNegative();
	else if( mode == "bench" )
		result = runBench();
	else if( !mode.empty() )
	{
		std::fprintf( stderr, "unknown mode --%s (try --help)\n", mode.c_str() );
		result = 2;
	}
	else
	{
		Rig rig( effect );
		rig.fps = fps;
		Floats card;
		if( effect && !clipPath.empty() )
		{
			std::ifstream file( clipPath, std::ios::binary );
			Bytes raw( static_cast< size_t >( width ) * height * 4 );
			if( !file.read( reinterpret_cast< char* >( raw.data() ), static_cast< std::streamsize >( raw.size() ) ) )
			{
				std::fprintf( stderr, "--clip %s: not %dx%d RGBA\n", clipPath.c_str(), width, height );
				return 2;
			}
			card.resize( raw.size() );
			for( int y = 0; y < height; ++y )
				for( int x = 0; x < width * 4; ++x )
					card[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ] = raw[ static_cast< size_t >( y ) * width * 4 + x ] / 255.0f;
		}
		else if( effect )
			card = buildCard( width, height );
		if( !rig.Init( width, height, effect ? &card : nullptr ) )
			result = 1;
		else
		{
			for( const std::string& setting : settings )
			{
				std::string error;
				if( !applySetting( rig.plugin, setting, error ) )
				{
					std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
					return 2;
				}
			}
			if( beat )
				rig.feed = AudioFeed::Pulses;
			if( !rig.Render( std::max( frames, 1 ) ) )
				result = 1;
			else if( writePng( outPath, width, height, rig.Output() ) )
				std::printf( "wrote %s -- %dx%d, %d frames at %g fps (%.2f s)\n", outPath.c_str(), width, height, frames, fps, frames / fps );
			else
				result = 1;
		}
	}

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return result;
}
