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
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
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
	case FF_TYPE_FILE: return "file";
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

	/// The whole phosphor, all four components: (flash, afterglow) of
	/// everything, then of the sprites' share.
	Floats Phosphor4() const
	{
		Floats data( static_cast< size_t >( kBearings ) * kRanges * 4 );
		glBindTexture( GL_TEXTURE_2D, plugin.PhosphorTextureID() );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glGetTexImage( GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, data.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		return data;
	}

	/// One rectangle of the phosphor, all four components, row by row.
	Floats PhosphorRect4( int x, int y, int w, int h )
	{
		if( !readFBO )
			glGenFramebuffers( 1, &readFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, readFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, plugin.PhosphorTextureID(), 0 );
		Floats data( static_cast< size_t >( w ) * h * 4 );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( x, y, w, h, GL_RGBA, GL_FLOAT, data.data() );
		glBindFramebuffer( GL_FRAMEBUFFER, 0 );
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
			//A file parameter takes its path as text, as the host hands it.
			if( parameter.type == FF_TYPE_FILE || parameter.type == FF_TYPE_TEXT )
				plugin.SetTextParameter( parameter.index, value.c_str() );
			else
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

std::string fixturePath( const std::string& name );
void setSpriteFile( Rig& rig, const std::string& path );

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
	std::printf( "\n=== bench: the defaults, after 120 frames of warm-up, each frame timed with glFinish both sides\n"
	             "    (then with 16 sprites of tools/fixtures/alien.png, growing on approach, the sprites' own defaults otherwise,\n"
	             "    Closing and then Transit)\n" );
	for( int sprites : { 0, 16, -16 } )
	for( bool effect : { false, true } )
		for( const Size& size : sizes )
		{
			Rig rig( effect );
			if( !rig.Init( size.w, size.h ) )
				return 1;
			if( sprites != 0 )
			{
				rig.Set( PT_SPRITE_COUNT, static_cast< float >( std::abs( sprites ) ) );
				rig.Set( PT_SPRITE_GROW, 0.5f );
				if( sprites < 0 )
					rig.Set( PT_SPRITE_PATH, static_cast< float >( SpritePath::Transit ) );
				setSpriteFile( rig, fixturePath( "alien.png" ) );
			}
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
			std::printf( "  %-14s %-19s %-6s median %5.2f ms/frame, mean %5.2f, worst %5.2f  (median %4.1f%% of a 60 fps frame)\n",
			             effect ? "SW Radar Over" : "SW Radar", sprites > 0 ? "16 sprites Closing" : sprites < 0 ? "16 sprites Transit" : "",
			             size.name, times[ kTimed / 2 ], mean, times.back(),
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
	// Sprites (radar#5).
	bool spriteUtf16Bytes         = false;///< --sprite-utf16: every byte widened to a code unit
	bool spriteMotionPerFrame     = false;///< --sprite-motion: distance added per frame, not per second
	bool spriteKeepLast           = false;///< --sprite-file: a failed file leaves the last picture up
	bool spriteThresholdIgnored   = false;///< --sprite-threshold: any alpha at all is the target
	bool spriteOrientationIgnored = false;///< --sprites: Face Centre drawn upright
	bool spriteSpeedPerFrame      = false;///< --sprite-speed: the plugin's motion tied to frames
	bool spriteNoFade             = false;///< --sprite-beam: the sprites' share never fades
	bool spriteWallSeed           = false;///< --sprite-repeat: the seed salted from the wall clock
	bool spriteHueBackwards       = false;///< --sprite-colour: Hue turns the colours the other way
	// Since v0.2.0: Path, Start With, Head In, Sprite Colour.
	bool transitNorth        = false;///< --sprite-transit: Transit's tracks run north
	bool transitGrowByTravel = false;///< --sprite-transit: Transit grows by the share of its track done, not by range
	bool transitEntryWide    = false;///< --sprite-path: Transit's entry points across the whole width
	bool arrivalNaive        = false;///< --sprite-path, --sprite-arrivals: latecomers over one crossing at Sprite Speed
	bool headInAsFace        = false;///< --sprites: Head In drawn as Face Centre
	bool colourSwapped       = false;///< --sprite-colour: the picture's colour read as BGR
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
	                          "0 Contacts 2\n60 Contacts 12\n0 Audio Strobe 0\n60 Audio Strobe 1\n"
	                          "0 Sprite Count 2\n60 Sprite Count 12\n0 Orientation 0\n60 Orientation 1\n"
	                          "0 Stability 0\n60 Stability 1\n" );
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

//===========================================================================
// Sprites (radar#5): an image file as contacts the beam paints.
//===========================================================================
std::string fixturePath( const std::string& name )
{
	const char* env = std::getenv( "RATEST_FIXTURES" );
	return std::string( env != nullptr && *env != 0 ? env : RATEST_FIXTURES ) + "/" + name;
}

/// A scratch folder for this run's generated sprites, removed at exit.
const std::string& scratchFolder()
{
	struct Folder
	{
		std::string path;
		Folder()
		{
			const char* tmp     = std::getenv( "TMPDIR" );
			std::string pattern = ( tmp != nullptr && *tmp != 0 ) ? tmp : "/tmp";
			if( pattern.back() != '/' )
				pattern += '/';
			pattern += "ratest-sprites-XXXXXX";
			std::vector< char > buffer( pattern.begin(), pattern.end() );
			buffer.push_back( 0 );
			const char* made = mkdtemp( buffer.data() );
			path             = made ? made : "/tmp";
		}
		~Folder()
		{
			if( path != "/tmp" )
			{
				std::error_code ignored;
				std::filesystem::remove_all( path, ignored );
			}
		}
	};
	static Folder folder;
	return folder.path;
}

std::string readBytes( const std::string& path )
{
	std::ifstream file( path, std::ios::binary );
	return std::string( std::istreambuf_iterator< char >( file ), std::istreambuf_iterator< char >() );
}

bool writeBytes( const std::string& path, const std::string& bytes )
{
	std::ofstream file( path, std::ios::binary );
	file.write( bytes.data(), static_cast< std::streamsize >( bytes.size() ) );
	return static_cast< bool >( file );
}

/// A sprite PNG: `alpha( x, y )`, x right and y UP in pixels, over white (or red).
bool writeAlphaSprite( const std::string& path, int w, int h, const std::function< double( int, int ) >& alpha, bool red = false )
{
	Floats rgba( static_cast< size_t >( w ) * h * 4 );
	for( int y = 0; y < h; ++y )
		for( int x = 0; x < w; ++x )
		{
			float* o = &rgba[ ( static_cast< size_t >( y ) * w + x ) * 4 ];
			o[ 0 ]   = 1.0f;
			o[ 1 ]   = red ? 0.0f : 1.0f;
			o[ 2 ]   = red ? 0.0f : 1.0f;
			o[ 3 ]   = static_cast< float >( alpha( x, y ) );
		}
	return writePng( path, w, h, rgba );
}

/// Nothing but the sprites: no clutter, noise, land, traffic, graticule or
/// clip (the Over's Threshold at 1); STC flat; a 0.5-degree beam and a
/// 0.05 us pulse (7.5 m: a third of a range bin on 24 km), so a sprite's
/// shape is barely smeared; the sprites held still and steady.
void spriteScene( Rig& rig, double gainDb = -30.0 )
{
	quiet( rig );
	rig.Set( PT_THRESHOLD, 1.0f );
	rig.Set( PT_STC, 1.0f );
	rig.Set( PT_GAIN, ParamFromGainDb( gainDb ) );
	rig.Set( PT_SIDELOBES, 0.0f );
	rig.Set( PT_BEAMWIDTH, ParamFromBeamwidth( 0.5 ) );
	rig.Set( PT_PULSE, ParamFromPulse( 0.05 ) );
	rig.Set( PT_SPRITE_SPEED, 0.0f );
	rig.Set( PT_SPEED_SPREAD, 0.0f );
	rig.Set( PT_STABILITY, 1.0f );
	rig.Set( PT_SPRITE_GROW, 0.0f );
	rig.Set( PT_SPRITE_THRESHOLD, 0.5f );
}

void setSpriteFile( Rig& rig, const std::string& path )
{
	rig.plugin.SetTextParameter( static_cast< unsigned int >( rig.plugin.HostIndexOf( PT_SPRITE_FILE ) ), path.c_str() );
}

/// The bearing bin whose centre is nearest `bearing`.
int nearestColumn( double bearing )
{
	return sweep::Column( static_cast< int64_t >( std::lround( bearing / ( 2.0 * kPi / kBearings ) - 0.5 ) ), kBearings );
}

/// Luminance of the output, bilinear between pixel centres (--arc's reading).
double screenLuminance( const Floats& out, int width, int height, double x, double y )
{
	x -= 0.5;
	y -= 0.5;
	const int x0 = static_cast< int >( std::floor( x ) ), y0 = static_cast< int >( std::floor( y ) );
	const double fx = x - x0, fy = y - y0;
	double sum = 0.0;
	for( int dy = 0; dy < 2; ++dy )
		for( int dx = 0; dx < 2; ++dx )
		{
			const int xx = std::clamp( x0 + dx, 0, width - 1 ), yy = std::clamp( y0 + dy, 0, height - 1 );
			const float* p = &out[ ( static_cast< size_t >( yy ) * width + xx ) * 4 ];
			sum += ( 0.2126 * p[ 0 ] + 0.7152 * p[ 1 ] + 0.0722 * p[ 2 ] ) * ( dx ? fx : 1.0 - fx ) * ( dy ? fy : 1.0 - fy );
		}
	return sum;
}

double pixelLuminance( const Floats& out, int width, int x, int y )
{
	const float* p = &out[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
	return 0.2126 * p[ 0 ] + 0.7152 * p[ 1 ] + 0.0722 * p[ 2 ];
}

/// Connected regions of the phosphor's channel `channel` above `fraction` of
/// its maximum, 8-connected, wrapping in bearing.
int countPolarBlobs( const Floats& ph4, int channel, double fraction )
{
	float peak = 0.0f;
	for( size_t i = static_cast< size_t >( channel ); i < ph4.size(); i += 4 )
		peak = std::max( peak, ph4[ i ] );
	if( peak <= 0.0f )
		return 0;
	const float floor = static_cast< float >( fraction * peak );
	std::vector< unsigned char > seen( static_cast< size_t >( kBearings ) * kRanges, 0 );
	auto lit = [ & ]( int x, int y ) { return ph4[ ( static_cast< size_t >( y ) * kBearings + x ) * 4 + channel ] > floor; };
	int blobs = 0;
	std::vector< std::pair< int, int > > stack;
	for( int y = 0; y < kRanges; ++y )
		for( int x = 0; x < kBearings; ++x )
		{
			if( seen[ static_cast< size_t >( y ) * kBearings + x ] || !lit( x, y ) )
				continue;
			++blobs;
			stack.push_back( { x, y } );
			seen[ static_cast< size_t >( y ) * kBearings + x ] = 1;
			while( !stack.empty() )
			{
				const auto [ cx, cy ] = stack.back();
				stack.pop_back();
				for( int dy = -1; dy <= 1; ++dy )
					for( int dx = -1; dx <= 1; ++dx )
					{
						const int nx = ( cx + dx + kBearings ) % kBearings, ny = cy + dy;
						if( ny < 0 || ny >= kRanges || seen[ static_cast< size_t >( ny ) * kBearings + nx ] || !lit( nx, ny ) )
							continue;
						seen[ static_cast< size_t >( ny ) * kBearings + nx ] = 1;
						stack.push_back( { nx, ny } );
					}
			}
		}
	return blobs;
}

/// Connected regions of the output brighter than the scope's empty face by
/// `fraction` of the brightest, 4-connected, inside the scope.
int countScreenBlobs( const Rig& rig, const Floats& out, double fraction )
{
	const double cx = rig.plugin.ScopeCentreX(), cy = rig.plugin.ScopeCentreY(), radius = rig.plugin.ScopeRadius();
	auto inside = [ & ]( int x, int y ) { return std::hypot( x + 0.5 - cx, y + 0.5 - cy ) < radius - 1.5; };
	double face = 1e30, peak = -1e30;
	for( int y = 0; y < rig.height; ++y )
		for( int x = 0; x < rig.width; ++x )
			if( inside( x, y ) )
			{
				face = std::min( face, pixelLuminance( out, rig.width, x, y ) );
				peak = std::max( peak, pixelLuminance( out, rig.width, x, y ) );
			}
	const double floor = face + fraction * ( peak - face );
	std::vector< unsigned char > seen( static_cast< size_t >( rig.width ) * rig.height, 0 );
	auto lit = [ & ]( int x, int y ) { return inside( x, y ) && pixelLuminance( out, rig.width, x, y ) > floor; };
	int blobs = 0;
	std::vector< std::pair< int, int > > stack;
	for( int y = 0; y < rig.height; ++y )
		for( int x = 0; x < rig.width; ++x )
		{
			if( seen[ static_cast< size_t >( y ) * rig.width + x ] || !lit( x, y ) )
				continue;
			++blobs;
			stack.push_back( { x, y } );
			seen[ static_cast< size_t >( y ) * rig.width + x ] = 1;
			while( !stack.empty() )
			{
				const auto [ px, py ] = stack.back();
				stack.pop_back();
				const int nx[] = { px + 1, px - 1, px, px }, ny[] = { py, py, py + 1, py - 1 };
				for( int k = 0; k < 4; ++k )
				{
					if( nx[ k ] < 0 || ny[ k ] < 0 || nx[ k ] >= rig.width || ny[ k ] >= rig.height
					    || seen[ static_cast< size_t >( ny[ k ] ) * rig.width + nx[ k ] ] || !lit( nx[ k ], ny[ k ] ) )
						continue;
					seen[ static_cast< size_t >( ny[ k ] ) * rig.width + nx[ k ] ] = 1;
					stack.push_back( { nx[ k ], ny[ k ] } );
				}
			}
		}
	return blobs;
}

/// A sprite's extent in range, scope units, read off the phosphor's sprite
/// afterglow along the bearing bin nearest its centre: the half-maximum width
/// of the profile within `reach` of its centre (another sprite, say one over
/// the centre of the scope, can cross the same bin).
/// A sprite nothing else can reach into: no other sprite's square (at any
/// turn) overlaps its own, and neither the centre nor the rim cuts it.
bool isolated( const std::vector< RadarPlugin::SpriteDraw >& sprites, size_t i )
{
	const auto& s     = sprites[ i ];
	const double half = std::sqrt( 2.0 ) * std::max( s.hx, s.hy );
	if( s.range - half <= 0.0 || s.range + half >= 1.0 )
		return false;
	for( size_t j = 0; j < sprites.size(); ++j )
		if( j != i && std::hypot( s.x - sprites[ j ].x, s.y - sprites[ j ].y )
		                  <= half + std::sqrt( 2.0 ) * std::max( sprites[ j ].hx, sprites[ j ].hy ) )
			return false;
	return true;
}

double radialExtent( Rig& rig, const RadarPlugin::SpriteDraw& s, double reach )
{
	const Floats column = rig.PhosphorRect4( nearestColumn( s.bearing ), 0, 1, kRanges );
	const int first     = std::max( 0, static_cast< int >( ( s.range - reach ) * kRanges ) );
	const int last      = std::min( kRanges - 1, static_cast< int >( ( s.range + reach ) * kRanges ) );
	std::vector< double > profile;
	for( int k = first; k <= last; ++k )
		profile.push_back( column[ static_cast< size_t >( k ) * 4 + 3 ] );
	const Width w = halfMaxWidth( profile );
	return w.valid ? ( w.right - w.left ) / kRanges : 0.0;
}

//===========================================================================
// --sprite-utf16 (no GL): the Windows path, checked where it can be.
//===========================================================================
std::u16string utf16Of( const std::vector< char32_t >& points )
{
	std::u16string out;
	for( char32_t c : points )
		if( c >= 0x10000 )
		{
			out.push_back( static_cast< char16_t >( 0xD800 + ( ( c - 0x10000 ) >> 10 ) ) );
			out.push_back( static_cast< char16_t >( 0xDC00 + ( ( c - 0x10000 ) & 0x3FF ) ) );
		}
		else
			out.push_back( static_cast< char16_t >( c ) );
	return out;
}

int runSpriteUtf16( const Perturb& perturb )
{
	std::printf( "\n=== sprite-utf16: a sprite path in UTF-8 widens to the UTF-16 that _wfopen takes on Windows; a malformed one is "
	             "refused\n" );
	auto convert = [ & ]( const std::string& in, std::u16string& out ) {
		if( perturb.spriteUtf16Bytes )
		{
			out = sprite::WidenBytesForTest( in );
			return true;
		}
		return sprite::Utf8ToUtf16( in, out );
	};
	auto ascii = []( const std::string& s ) { return std::vector< char32_t >( s.begin(), s.end() ); };
	auto join  = []( std::vector< std::vector< char32_t > > parts ) {
		std::vector< char32_t > all;
		for( const auto& p : parts )
			all.insert( all.end(), p.begin(), p.end() );
		return all;
	};
	struct Good
	{
		std::string utf8;
		std::vector< char32_t > points;
		const char* what;
	};
	const std::vector< Good > good = {
		{ "C:\\Users\\Steve\\Pictures\\skull.png", ascii( "C:\\Users\\Steve\\Pictures\\skull.png" ), "ASCII" },
		{ std::string( "C:\\Users\\Zo" ) + "\xC3\xAB" + "\\Bureau\\" + "\xF0\x9F\x91\xBD" + ".png",
		  join( { ascii( "C:\\Users\\Zo" ), { 0xEB }, ascii( "\\Bureau\\" ), { 0x1F47D }, ascii( ".png" ) } ),
		  "Latin-1 and an emoji past the BMP (a surrogate pair)" },
		{ std::string( "D:\\" ) + "\xE3\x83\x91\xE3\x83\xB3\xE3\x83\x80" + "\\" + "\xF0\x9F\x90\xBC" + ".jpg",
		  join( { ascii( "D:\\" ), { 0x30D1, 0x30F3, 0x30C0 }, ascii( "\\" ), { 0x1F43C }, ascii( ".jpg" ) } ),
		  "katakana and another emoji" },
	};
	int wrong = 0;
	std::string what;
	for( const Good& g : good )
	{
		std::u16string out;
		const bool ok = convert( g.utf8, out ) && out == utf16Of( g.points );
		wrong += !ok;
		what += fmt( " %s: %s;", g.what, ok ? "exact" : "WRONG" );
	}
	Check( wrong == 0, fmt( "%zu paths, %d wrong:%s", good.size(), wrong, what.c_str() ) );

	const std::vector< std::pair< std::string, const char* > > bad = {
		{ "\xC3", "a truncated two-byte sequence" },
		{ "\xE2\x82", "a truncated three-byte sequence" },
		{ "\xC0\xAF", "an overlong '/'" },
		{ std::string( "\xED\xA0\x80" ), "a UTF-8-encoded surrogate" },
		{ "\xF4\x90\x80\x80", "a code point past U+10FFFF" },
		{ std::string( "\x80" ) + "abc", "a lone continuation byte" },
	};
	int accepted = 0;
	what.clear();
	for( const auto& b : bad )
	{
		std::u16string out;
		const bool took = convert( b.first, out );
		accepted += took;
		if( took )
			what += fmt( " %s accepted;", b.second );
	}
	Check( accepted == 0, fmt( "%zu malformed paths, %d accepted (refused, never guessed at)%s", bad.size(), accepted, what.c_str() ) );
	return Verdict();
}

//===========================================================================
// --sprite-motion (no GL): Swarm.cpp's law, on the CPU, in double.
//===========================================================================
std::vector< double > frameTimes( double fps, double seconds, bool jitter )
{
	std::vector< double > t;
	if( !jitter )
	{
		for( int n = 0; n / fps <= seconds + 1e-12; ++n )
			t.push_back( n / fps );
		return t;
	}
	double now = 0.0;
	for( uint32_t n = 0; now <= seconds; ++n )
	{
		t.push_back( now );
		now += 0.008 + 0.022 * hash01( n, 77 );
	}
	return t;
}

int runSpriteMotion( const Perturb& perturb )
{
	std::printf( "\n=== sprite-motion: sprites close on the centre at Sprite Speed and start again at the rim, the same at any frame "
	             "rate\n" );
	using swarm::Settings;
	using swarm::Swarm;
	struct Clock
	{
		const char* name;
		std::vector< double > t;
	};

	//1. Steady: Stability 1, Spread 0. The range is 1 - start - v t, to the
	//centre at t* = (1 - start) / v, then 1 - v ( t - t_r ) from the frame
	//t_r it was reset on, the first at or after t*. Double sums of frame
	//lengths: 1e-9 is a thousand times their rounding.
	{
		const double v = 0.1;
		Settings s;
		s.count     = 1;
		s.seed      = 11;
		s.speed     = v;
		s.stability = 1.0;
		const double r0 = 1.0 - Swarm::StartOf( 0 ), tStar = r0 / v;
		const std::vector< Clock > clocks = { { "60 fps", frameTimes( 60.0, 14.0, false ) },
			                                  { "24 fps", frameTimes( 24.0, 14.0, false ) },
			                                  { "59.94 fps", frameTimes( 59.94, 14.0, false ) },
			                                  { "jittered 8-30 ms", frameTimes( 0.0, 14.0, true ) } };
		for( const Clock& clock : clocks )
		{
			Swarm sw;
			sw.SetPerFrameForTest( perturb.spriteMotionPerFrame );
			double worst = 0.0, respawnAt = -1.0, longest = 0.0;
			uint32_t generation = 0;
			for( size_t i = 0; i < clock.t.size(); ++i )
			{
				const double dt = i == 0 ? 0.0 : clock.t[ i ] - clock.t[ i - 1 ];
				longest         = std::max( longest, dt );
				sw.Step( dt, s );
				const swarm::Placed p = sw.Sprites()[ 0 ];
				if( p.generation != generation )
				{
					generation = p.generation;
					if( generation == 1 )
						respawnAt = clock.t[ i ];
				}
				const double want = generation == 0 ? r0 - v * clock.t[ i ] : 1.0 - v * ( clock.t[ i ] - respawnAt );
				worst             = std::max( worst, std::fabs( p.range - want ) );
			}
			const bool onTime = respawnAt >= tStar - 1e-9 && respawnAt < tStar + longest + 1e-9;
			Check( worst < 1e-9 && onTime && generation == 1,
			       fmt( "%-16s from r = %.4f at %.3f radii/s: off the law by at most %.1e; reached the centre at %.4f s "
			            "(t* = %.4f, within a frame: %s) and started again at the rim",
			            clock.name, r0, v, worst, respawnAt, tStar, onTime ? "yes" : "NO" ) );
		}
	}

	//2. Speed Spread: each sprite at its own speed, 1/3x to 3x at 1, all at
	//Sprite Speed at 0.
	{
		const double v = 0.1;
		for( double spread : { 0.0, 1.0 } )
		{
			Settings s;
			s.count     = 32;
			s.seed      = 11;
			s.speed     = v;
			s.spread    = spread;
			s.stability = 1.0;
			Swarm sw;
			sw.Step( 0.0, s );
			const auto a = sw.Sprites();
			sw.Step( 0.5, s );
			const auto b = sw.Sprites();
			double lo = 1e30, hi = 0.0, worst = 0.0;
			int measured = 0;
			for( size_t i = 0; i < a.size(); ++i )
			{
				if( a[ i ].generation != b[ i ].generation )
					continue;
				const double rate = ( a[ i ].range - b[ i ].range ) / 0.5;
				worst             = std::max( worst, std::fabs( rate - Swarm::SpeedOf( s, static_cast< uint32_t >( i ), 0 ) ) );
				lo                = std::min( lo, rate );
				hi                = std::max( hi, rate );
				++measured;
			}
			const bool range = spread == 0.0 ? ( std::fabs( lo - v ) < 1e-12 && std::fabs( hi - v ) < 1e-12 )
			                                 : ( lo >= v / 3.0 - 1e-12 && hi <= 3.0 * v + 1e-12 && hi / lo > 3.0 );
			Check( measured >= 24 && worst < 1e-12 && range,
			       fmt( "Speed Spread %.0f: %d sprites, each at its own speed to %.1e; slowest %.4f, fastest %.4f radii/s "
			            "(Sprite Speed %.2f: %s)",
			            spread, measured, worst, lo, hi, v, spread == 0.0 ? "all at it" : "1/3x to 3x, and really spread" ) );
		}
	}

	//3. Stability 0: the speed wanders and the bearing weaves -- and the
	//distance is the closed-form integral, so the frame rate cannot move a
	//sprite. A slot that reached the centre is left out: the overshoot it
	//drops depends on the frame.
	{
		Settings s;
		s.count     = 8;
		s.seed      = 11;
		s.speed     = 0.05;
		s.spread    = 0.5;
		s.stability = 0.0;
		const double T = 6.0;
		std::vector< Clock > clocks = { { "60 fps", frameTimes( 60.0, T, false ) },
			                            { "24 fps", frameTimes( 24.0, T, false ) },
			                            { "240 fps", frameTimes( 240.0, T, false ) },
			                            { "jittered", frameTimes( 0.0, T - 0.031, true ) } };
		clocks.back().t.push_back( T );
		std::vector< std::vector< swarm::Placed > > ends;
		double fastest = 0.0, slowest = 1e30, bearingSwing = 0.0;
		for( size_t c = 0; c < clocks.size(); ++c )
		{
			Swarm sw;
			sw.SetPerFrameForTest( perturb.spriteMotionPerFrame );
			double firstBearing = 0.0, lastRange = 0.0;
			for( size_t i = 0; i < clocks[ c ].t.size(); ++i )
			{
				const double dt = i == 0 ? 0.0 : clocks[ c ].t[ i ] - clocks[ c ].t[ i - 1 ];
				sw.Step( dt, s );
				const swarm::Placed p = sw.Sprites()[ 0 ];
				if( c == 0 && i > 0 && p.generation == 0 )
				{
					const double rate = ( lastRange - p.range ) / dt;
					fastest           = std::max( fastest, rate );
					slowest           = std::min( slowest, rate );
					bearingSwing      = std::max( bearingSwing, std::fabs( p.bearing - firstBearing ) );
				}
				if( i == 0 )
					firstBearing = p.bearing;
				lastRange = p.range;
			}
			ends.push_back( sw.Sprites() );
		}
		double worst = 0.0;
		int compared = 0;
		for( size_t slot = 0; slot < ends[ 0 ].size(); ++slot )
		{
			bool still = true;
			for( const auto& e : ends )
				still = still && e[ slot ].generation == 0;
			if( !still )
				continue;
			++compared;
			for( const auto& e : ends )
				worst = std::max( { worst, std::fabs( e[ slot ].x - ends[ 0 ][ slot ].x ), std::fabs( e[ slot ].y - ends[ 0 ][ slot ].y ) } );
		}
		Check( compared >= 4 && worst < 1e-9,
		       fmt( "Stability 0, 8 sprites after %.0f s at 60, 24, 240 fps and jittered: %d compared, positions agree to %.1e "
		            "(bound 1e-9)",
		            T, compared, worst ) );
		Check( slowest > 0.0 && fastest / slowest > 1.3 && bearingSwing > 0.02,
		       fmt( "Stability 0 wanders: one sprite's speed between %.4f and %.4f radii/s, its bearing through %.3f rad", slowest,
		            fastest, bearingSwing ) );
	}

	//4. The same seed and clock: the same sprites, to the bit. And a count
	//change keeps the sprites that remain.
	{
		Settings s;
		s.count     = 8;
		s.seed      = 11;
		s.speed     = 0.05;
		s.spread    = 0.5;
		s.stability = 0.3;
		const std::vector< double > t = frameTimes( 0.0, 10.0, true );
		Swarm a, b;
		int differ = 0;
		for( size_t i = 0; i < t.size(); ++i )
		{
			const double dt = i == 0 ? 0.0 : t[ i ] - t[ i - 1 ];
			a.Step( dt, s );
			b.Step( dt, s );
			const auto pa = a.Sprites(), pb = b.Sprites();
			for( size_t k = 0; k < pa.size(); ++k )
				differ += pa[ k ].x != pb[ k ].x || pa[ k ].y != pb[ k ].y || pa[ k ].generation != pb[ k ].generation;
		}
		Swarm c;
		s.count = 5;
		c.Step( 0.0, s );
		c.Step( 1.0, s );
		const auto five = c.Sprites();
		s.count = 9;
		c.Step( 0.0, s );
		const auto nine = c.Sprites();
		s.count = 3;
		c.Step( 0.0, s );
		const auto three = c.Sprites();
		bool kept = five.size() == 5 && nine.size() == 9 && three.size() == 3;
		for( size_t k = 0; kept && k < 5; ++k )
			kept = nine[ k ].x == five[ k ].x && nine[ k ].y == five[ k ].y && ( k >= 3 || ( three[ k ].x == five[ k ].x && three[ k ].y == five[ k ].y ) );
		for( size_t k = 5; kept && k < 9; ++k )
			kept = nine[ k ].range == 1.0 - Swarm::StartOf( static_cast< uint32_t >( k ) );
		Check( differ == 0 && kept,
		       fmt( "two swarms on one jittered clock (%zu frames): %d positions differ; Sprite Count 5 -> 9 -> 3 keeps the "
		            "sprites that remain and starts the new ones staggered: %s",
		            t.size(), differ, kept ? "yes" : "NO" ) );
	}
	return Verdict();
}

//===========================================================================
// --sprite-file: what a file becomes, and the blip when it cannot be one.
//===========================================================================
bool sameMask( const sprite::Mask& a, const sprite::Mask& b )
{
	return a.width == b.width && a.height == b.height && a.value == b.value;
}

int runSpriteFile( const Perturb& perturb )
{
	std::printf( "\n=== sprite-file: a PNG's alpha or a JPG's brightest channel is the sprite; no file, a bad one or a missing one "
	             "is the built-in blip\n" );
	const std::string dir = scratchFolder();
	auto alphaOf          = []( int x, int y ) { return ( x * 5 + y * 11 ) % 256; };
	const std::string gradient = dir + "/gradient.png";
	writeAlphaSprite( gradient, 40, 24, [ & ]( int x, int y ) { return alphaOf( x, y ) / 255.0; }, true );
	const std::string big = dir + "/big.png";
	double bigMean        = 0.0;
	writeAlphaSprite( big, 300, 200, [ & ]( int x, int y ) {
		const double a = std::hypot( x + 0.5 - 150.0, y + 0.5 - 100.0 ) < 80.0 ? 1.0 : 0.0;
		bigMean += a / ( 300.0 * 200.0 );
		return a;
	} );
	const std::string alienPng = fixturePath( "alien.png" ), alienJpg = fixturePath( "alien.jpg" ), broken = fixturePath( "broken.png" );
	const std::string truncated = dir + "/truncated.png";
	writeBytes( truncated, readBytes( alienPng ).substr( 0, 100 ) );
	//Non-ASCII all the way down: a folder and a name with accents, a check
	//mark, katakana, an eszett and an emoji past the BMP.
	const std::string folder = dir + "/sprit\xC3\xAB" + "s \xE2\x9C\x93 \xF0\x9F\x91\xBD";
	std::filesystem::create_directories( std::filesystem::u8path( folder ) );
	const std::string unicode = folder + "/\xE1\xBA\x9E" + "kull \xE3\x83\x91\xE3\x83\xB3\xE3\x83\x80.png";
	writeBytes( unicode, readBytes( alienPng ) );

	Rig rig;
	if( !rig.Init( kRasters.back().w, kRasters.back().h ) )
		return 1;
	spriteScene( rig );
	rig.Set( PT_SPRITE_COUNT, 3.0f );
	rig.plugin.SetSpriteKeepLastOnFailureForTest( perturb.spriteKeepLast );
	using Source = RadarPlugin::SpriteSource;
	auto load    = [ & ]( const std::string& path ) {
		setSpriteFile( rig, path );
		rig.Render( 1 );
		return rig.plugin.SpriteMaskForTest();
	};
	auto has = [ & ]( const char* text ) { return rig.plugin.SpriteWhyForTest().find( text ) != std::string::npos; };

	sprite::Mask m = load( "" );
	Check( rig.plugin.SpriteSourceForTest() == Source::BuiltIn && sameMask( m, sprite::BuiltIn() ),
	       fmt( "no file: the built-in blip, %dx%d", m.width, m.height ) );

	m        = load( gradient );
	int off  = 0;
	for( int y = 0; y < 24; ++y )
		for( int x = 0; x < 40; ++x )
			off += m.width != 40 || m.value[ static_cast< size_t >( y ) * 40 + x ] != static_cast< float >( alphaOf( x, y ) ) / 255.0f;
	Check( rig.plugin.SpriteSourceForTest() == Source::File && m.fromAlpha && m.width == 40 && m.height == 24 && off == 0,
	       fmt( "a 40x24 PNG whose alpha is a known pattern over red: its mask is the alpha, bottom row first, %d of 960 texels "
	            "wrong (red's brightest channel would be 1 everywhere)",
	            off ) );

	m           = load( big );
	double mean = 0.0;
	for( float v : m.value )
		mean += v / static_cast< double >( m.value.size() );
	Check( rig.plugin.SpriteSourceForTest() == Source::File && m.width == 128 && m.height == 85 && std::fabs( mean - bigMean ) < 1e-5,
	       fmt( "a 300x200 PNG is box-filtered to %dx%d (128 on the longer side) keeping the coverage: mean %.6f against %.6f", m.width,
	            m.height, mean, bigMean ) );

	const sprite::Mask alien = load( alienPng );
	const bool alienOk       = rig.plugin.SpriteSourceForTest() == Source::File && alien.fromAlpha && alien.width == 96;
	m                        = load( alienJpg );
	int agree                = 0;
	for( size_t i = 0; i < m.value.size() && i < alien.value.size(); ++i )
		agree += ( m.value[ i ] > 0.5f ) == ( alien.value[ i ] > 0.5f );
	Check( alienOk && rig.plugin.SpriteSourceForTest() == Source::File && !m.fromAlpha && m.width == 96
	           && agree > 0.97 * static_cast< double >( m.value.size() ),
	       fmt( "tools/fixtures/alien.png is read by its alpha; alien.jpg (the same head on black, no alpha) by its brightest "
	            "channel, and the two masks agree over 0.5 on %.1f%% of texels",
	            100.0 * agree / std::max< size_t >( m.value.size(), 1 ) ) );

	m = load( broken );
	Check( rig.plugin.SpriteSourceForTest() == Source::Fallback && sameMask( m, sprite::BuiltIn() ) && has( "will not decode" ),
	       fmt( "broken.png (text with a .png name): the blip, and the log says why: \"%s\"", rig.plugin.SpriteWhyForTest().c_str() ) );
	m = load( dir + "/no such sprite.png" );
	Check( rig.plugin.SpriteSourceForTest() == Source::Fallback && sameMask( m, sprite::BuiltIn() ) && has( "cannot be opened" ),
	       fmt( "a missing file: the blip, \"%s\"", rig.plugin.SpriteWhyForTest().c_str() ) );
	m = load( truncated );
	Check( rig.plugin.SpriteSourceForTest() == Source::Fallback && sameMask( m, sprite::BuiltIn() ),
	       fmt( "the first 100 bytes of alien.png: the blip, \"%s\"", rig.plugin.SpriteWhyForTest().c_str() ) );
	m = load( unicode );
	Check( rig.plugin.SpriteSourceForTest() == Source::File && sameMask( m, alien ),
	       "a copy of alien.png in a folder and under a name with accents, a check mark, katakana and an emoji: the same mask "
	       "(UTF-8 straight through on macOS; Windows widens it, --sprite-utf16)" );

	//The picture's colour (Sprite Colour's Picture, since v0.2.0): the mean of
	//its RGB, each pixel weighted by its mask value. A picture whose answer is
	//known to the bit: a red block (opaque), a blue-ish block (alpha 128), and
	//the rest transparent WHITE, so a mean that let transparent pixels in, or
	//weighted them wrong, would show.
	{
		const std::string blocks = dir + "/blocks.png";
		const int bw = 40, bh = 24;
		Floats rgba( static_cast< size_t >( bw ) * bh * 4 );
		double sum[ 3 ] = { 0.0, 0.0, 0.0 }, weight = 0.0;
		for( int y = 0; y < bh; ++y )
			for( int x = 0; x < bw; ++x )
			{
				const int px[ 4 ] = { x < 16 ? 255 : x < 28 ? 0 : 255, x < 16 ? 0 : x < 28 ? 64 : 255, x < 16 ? 0 : 255,
					                  x < 16 ? 255 : x < 28 ? 128 : 0 };
				for( int c = 0; c < 4; ++c )
					rgba[ ( static_cast< size_t >( y ) * bw + x ) * 4 + c ] = static_cast< float >( px[ c ] / 255.0 );
				const double w = px[ 3 ] / 255.0;
				for( int c = 0; c < 3; ++c )
					sum[ c ] += w * px[ c ] / 255.0;
				weight += w;
			}
		writePng( blocks, bw, bh, rgba );
		m          = load( blocks );
		double off = 0.0;
		for( int c = 0; c < 3; ++c )
			off = std::max( off, std::fabs( m.colour[ c ] - sum[ c ] / weight ) );
		Check( m.coloured && off < 1e-6,
		       fmt( "a red block, a half-transparent blue one and transparent white: its colour is the alpha-weighted mean (%.5f, %.5f, "
		            "%.5f), to %.1e",
		            sum[ 0 ] / weight, sum[ 1 ] / weight, sum[ 2 ] / weight, off ) );
	}
	{
		//make_fixtures.py paints every pixel of the alien (0.45, 0.95, 0.35),
		//edges included (it stores straight colour): (115, 242, 89) in 8 bits.
		const double want[ 3 ] = { 115.0 / 255.0, 242.0 / 255.0, 89.0 / 255.0 };
		m                      = load( alienPng );
		double off             = 0.0;
		for( int c = 0; c < 3; ++c )
			off = std::max( off, std::fabs( m.colour[ c ] - want[ c ] ) );
		const bool alienColour = m.coloured && off < 1e-6;
		m                      = load( alienJpg );
		const bool jpgGreen    = m.coloured && m.colour[ 1 ] > m.colour[ 0 ] && m.colour[ 1 ] > m.colour[ 2 ];
		m                      = load( fixturePath( "panda.png" ) );
		const bool pandaGrey   = !m.coloured;
		m                      = load( "" );
		Check( alienColour && jpgGreen && pandaGrey && !m.coloured,
		       fmt( "alien.png's colour is (115, 242, 89) / 255 to %.1e; alien.jpg's (by its brightest channel) is green too; the "
		            "black-and-white panda is grey and the blip has none, so neither tints anything",
		            off ) );
	}

	//The picture: a bad file draws exactly what no file does; a good one not.
	for( const Raster& raster : kRasters )
	{
		Floats pictures[ 3 ];
		const std::string paths[ 3 ] = { "", broken, alienPng };
		for( int k = 0; k < 3; ++k )
		{
			Rig r;
			if( !r.Init( raster.w, raster.h ) )
				return 1;
			spriteScene( r, 0.0 );
			r.plugin.SetSpriteKeepLastOnFailureForTest( perturb.spriteKeepLast );
			r.Set( PT_SPRITE_COUNT, 6.0f );
			r.Set( PT_SPRITE_SIZE, ParamFromSpriteSize( 0.2 ) );
			r.Set( PT_SPRITE_SPEED, ParamFromSpriteSpeed( 0.05 ) );
			setSpriteFile( r, paths[ k ] );
			if( !r.Render( 60 ) )
				return 1;
			pictures[ k ] = r.Output();
		}
		Check( pictures[ 0 ] == pictures[ 1 ] && pictures[ 0 ] != pictures[ 2 ],
		       fmt( "%dx%d  60 frames: broken.png draws exactly what no file does (the blip), alien.png something else", raster.w,
		            raster.h ) );
	}
	return Verdict();
}

//===========================================================================
// --sprite-threshold: the blip's disc is Sprite Size x (1 - Threshold) across.
//===========================================================================
/// The built-in blip as GL samples it: the stated cone at texel centres,
/// bilinear between them, clamped at the edge. The harness's own model of
/// the texture, to say how far its reconstruction moves the edge.
double blipBilinear( double s, double t )
{
	constexpr int n = 64;
	auto texel      = [ & ]( int i, int j ) {
		i = std::clamp( i, 0, n - 1 );
		j = std::clamp( j, 0, n - 1 );
		const double u = ( i + 0.5 ) / n - 0.5, v = ( j + 0.5 ) / n - 0.5;
		return std::max( 0.0, 1.0 - std::sqrt( u * u + v * v ) / 0.5 );
	};
	const double x = s * n - 0.5, y = t * n - 0.5;
	const int x0 = static_cast< int >( std::floor( x ) ), y0 = static_cast< int >( std::floor( y ) );
	const double fx = x - x0, fy = y - y0;
	return ( texel( x0, y0 ) * ( 1 - fx ) + texel( x0 + 1, y0 ) * fx ) * ( 1 - fy )
	       + ( texel( x0, y0 + 1 ) * ( 1 - fx ) + texel( x0 + 1, y0 + 1 ) * fx ) * fy;
}

int runSpriteThreshold( const Perturb& perturb )
{
	std::printf( "\n=== sprite-threshold: the built-in blip, a cone, is a disc Sprite Size x (1 - Threshold) across in range\n" );
	const double size = SpriteSizeFromParam( ParamFromSpriteSize( 0.4 ) );
	for( const Raster& raster : kRasters )
		for( double threshold : { 0.2, 0.5, 0.8 } )
		{
			Rig rig;
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			spriteScene( rig );
			rig.plugin.SetDecayForTest( false );
			rig.plugin.SetSpriteThresholdIgnoredForTest( perturb.spriteThresholdIgnored );
			rig.Set( PT_RPM, ParamFromRpm( 120.0 ) );//one rotation in 30 frames
			rig.Set( PT_SPRITE_COUNT, 1.0f );
			rig.Set( PT_SPRITE_SIZE, ParamFromSpriteSize( 0.4 ) );
			rig.Set( PT_SPRITE_THRESHOLD, static_cast< float >( threshold ) );
			if( !rig.Render( 31 ) )
				return 1;
			const RadarPlugin::SpriteDraw sp = rig.plugin.SpritesForTest()[ 0 ];
			const int col                    = nearestColumn( sp.bearing );
			const Floats column              = rig.PhosphorRect4( col, 0, 1, kRanges );
			std::vector< double > profile;
			for( int i = 0; i < kRanges; ++i )
				profile.push_back( column[ static_cast< size_t >( i ) * 4 + 3 ] );
			const Width w     = halfMaxWidth( profile );
			const double meas = w.valid ? ( w.right - w.left ) / kRanges : 0.0;
			const double law  = size * ( 1.0 - threshold );

			//Where the bilinear texture crosses the threshold along this
			//column's ray: the reconstruction's own error, which is part of
			//the bound, stated rather than fitted.
			const double b = bearingOfBin( col ), ux = std::sin( b ), uy = std::cos( b );
			const double along = sp.x * ux + sp.y * uy;
			auto valueAt       = [ & ]( double rho ) {
				return blipBilinear( ( rho * ux - sp.x ) / size + 0.5, ( rho * uy - sp.y ) / size + 0.5 );
			};
			auto edge = [ & ]( double dir ) {
				double lo = along, hi = along + dir * 0.5 * size;
				for( int i = 0; i < 60; ++i )
				{
					const double mid = 0.5 * ( lo + hi );
					( valueAt( mid ) > threshold ? lo : hi ) = mid;
				}
				return 0.5 * ( lo + hi );
			};
			const double model = edge( 1.0 ) - edge( -1.0 );
			//Per edge: half a range bin (the edge is sampled at bin centres,
			//and the 2-tap pulse average makes a one-bin ramp) and a quarter
			//bin for the 0.5-degree beam's average over neighbouring columns,
			//whose chords are shorter. Plus the texture's own reconstruction.
			const double bin = 1.0 / kRanges;
			const double tol = 2.0 * 0.75 * bin + std::fabs( model - law );
			Check( w.valid && std::fabs( meas - law ) <= tol,
			       fmt( "%dx%d  Threshold %.1f: %.5f across (%.1f bins) against %.4f x (1 - T) = %.5f; error %.2f bins, bound %.2f "
			            "(1.5 for the sampling and the beam, %.2f the bilinear texture's)",
			            raster.w, raster.h, threshold, meas, meas * kRanges, size, law, std::fabs( meas - law ) / bin, tol / bin,
			            std::fabs( model - law ) / bin ) );
		}
	return Verdict();
}

//===========================================================================
// --sprites: how many, how big, which way up, and growing -- from the picture.
//===========================================================================
const char* orientationName( Orientation o )
{
	return o == Orientation::FaceCentre ? "Face Centre" : o == Orientation::HeadIn ? "Head In" : "Upright";
}
const char* orientationWay( Orientation o )
{
	return o == Orientation::FaceCentre ? "outward" : o == Orientation::HeadIn ? "inward" : "screen up";
}

/// The light of each isolated sprite on the screen, against where its
/// orientation says its light should sit: `shift` of its size along its up
/// from its middle (a quarter for the top-half square).
struct OrientationReading
{
	int measured = 0, offNorth = 0;
	std::vector< double > angles, ratios;
	double worstAngle = 0.0, worstLength = 0.0;
	int wrong( double degrees, double share ) const
	{
		int bad = 0;
		for( size_t i = 0; i < angles.size(); ++i )
			bad += angles[ i ] > degrees || std::fabs( ratios[ i ] - 1.0 ) > share;
		return bad;
	}
};

OrientationReading readOrientation( Rig& rig, double size, Orientation orientation, double shift )
{
	OrientationReading o;
	const Floats out = rig.Output();
	const double cx = rig.plugin.ScopeCentreX(), cy = rig.plugin.ScopeCentreY(), R = rig.plugin.ScopeRadius();
	const auto sprites = rig.plugin.SpritesForTest();
	double face        = 1e30;
	for( int y = 0; y < rig.height; ++y )
		for( int x = 0; x < rig.width; ++x )
			if( std::hypot( x + 0.5 - cx, y + 0.5 - cy ) < R - 2.0 )
				face = std::min( face, pixelLuminance( out, rig.width, x, y ) );
	for( size_t i = 0; i < sprites.size(); ++i )
	{
		const auto& s = sprites[ i ];
		if( !isolated( sprites, i ) )
			continue;
		const double px = cx + s.x * R, py = cy + s.y * R, reach = 0.75 * size * R;
		double sum = 0.0, mx = 0.0, my = 0.0;
		for( int y = static_cast< int >( py - reach ) - 1; y <= static_cast< int >( py + reach ) + 1; ++y )
			for( int x = static_cast< int >( px - reach ) - 1; x <= static_cast< int >( px + reach ) + 1; ++x )
			{
				if( x < 0 || y < 0 || x >= rig.width || y >= rig.height )
					continue;
				const double qx = x + 0.5, qy = y + 0.5;
				if( std::hypot( qx - px, qy - py ) > reach )
					continue;
				//Each pixel belongs to the nearest sprite.
				bool mine = true;
				for( size_t j = 0; j < sprites.size(); ++j )
					mine = mine && ( j == i || std::hypot( qx - ( cx + sprites[ j ].x * R ), qy - ( cy + sprites[ j ].y * R ) )
					                               >= std::hypot( qx - px, qy - py ) );
				if( !mine )
					continue;
				const double l = pixelLuminance( out, rig.width, x, y ) - face;
				sum += l;
				mx += l * ( qx - px );
				my += l * ( qy - py );
			}
		if( sum <= 0.0 )
			continue;
		++o.measured;
		mx /= sum;
		my /= sum;
		const double turn = orientation == Orientation::FaceCentre ? 1.0 : orientation == Orientation::HeadIn ? -1.0 : 0.0;
		const double ex   = turn != 0.0 ? turn * std::sin( s.bearing ) : 0.0;
		const double ey   = turn != 0.0 ? turn * std::cos( s.bearing ) : 1.0;
		const double length = std::hypot( mx, my );
		const double angle  = std::acos( std::clamp( ( mx * ex + my * ey ) / std::max( length, 1e-12 ), -1.0, 1.0 ) ) * 180.0 / kPi;
		const double ratio  = length / ( shift * size * R );
		o.angles.push_back( angle );
		o.ratios.push_back( ratio );
		o.worstAngle  = std::max( o.worstAngle, angle );
		o.worstLength = std::max( o.worstLength, std::fabs( ratio - 1.0 ) );
		const double fromNorth = std::fabs( std::remainder( s.bearing, 2.0 * kPi ) ) * 180.0 / kPi;
		o.offNorth += fromNorth > 30.0;
	}
	return o;
}

/// Where a mask's silhouette (bilinear, over `threshold`) has its centroid,
/// along its up, as a share of its box's longer side: the harness's own
/// reading of the picture the plugin loaded.
double silhouetteOffset( const sprite::Mask& m, double threshold )
{
	auto texel = [ & ]( int i, int j ) {
		i = std::clamp( i, 0, m.width - 1 );
		j = std::clamp( j, 0, m.height - 1 );
		return static_cast< double >( m.value[ static_cast< size_t >( j ) * m.width + i ] );
	};
	const int n = 512;
	double sum = 0.0, moment = 0.0;
	for( int b = 0; b < n; ++b )
		for( int a = 0; a < n; ++a )
		{
			const double s = ( a + 0.5 ) / n, tt = ( b + 0.5 ) / n;
			const double x = s * m.width - 0.5, y = tt * m.height - 0.5;
			const int x0 = static_cast< int >( std::floor( x ) ), y0 = static_cast< int >( std::floor( y ) );
			const double fx = x - x0, fy = y - y0;
			const double v = ( texel( x0, y0 ) * ( 1 - fx ) + texel( x0 + 1, y0 ) * fx ) * ( 1 - fy )
			                 + ( texel( x0, y0 + 1 ) * ( 1 - fx ) + texel( x0 + 1, y0 + 1 ) * fx ) * fy;
			if( v > threshold )
			{
				sum += 1.0;
				moment += tt - 0.5;
			}
		}
	const double longer = std::max( m.width, m.height );
	return sum > 0.0 ? moment / sum * m.height / longer : 0.0;
}

int runSprites( const Perturb& perturb )
{
	std::printf( "\n=== sprites: Sprite Count blobs on the phosphor and the screen; Size across them; their top where Orientation "
	             "says; Grow On Approach\n" );
	const std::string dir    = scratchFolder();
	const std::string square = dir + "/square.png", top = dir + "/top-half.png";
	//An opaque white square: no transparency, so the brightest channel (1).
	writeAlphaSprite( square, 16, 16, []( int, int ) { return 1.0; } );
	//Only its top half: the mask's centroid is a quarter of the size above
	//its centre, along the sprite's up.
	writeAlphaSprite( top, 32, 32, []( int, int y ) { return y >= 16 ? 1.0 : 0.0; } );

	for( const Raster& raster : kRasters )
	{
		//1. Count.
		for( int count : { 1, 5, 12 } )
		{
			Rig rig;
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			spriteScene( rig, 0.0 );
			rig.plugin.SetDecayForTest( false );
			rig.Set( PT_RPM, ParamFromRpm( 120.0 ) );
			rig.Set( PT_SPRITE_COUNT, static_cast< float >( count ) );
			rig.Set( PT_SPRITE_SIZE, ParamFromSpriteSize( 0.08 ) );
			if( !rig.Render( 31 ) )
				return 1;
			const int polar  = countPolarBlobs( rig.Phosphor4(), 3, 0.05 );
			const int screen = countScreenBlobs( rig, rig.Output(), 0.2 );
			Check( polar == count && screen == count,
			       fmt( "%dx%d  Sprite Count %d: %d blobs on the phosphor, %d on the screen", raster.w, raster.h, count, polar, screen ) );
		}

		//2. Size: the square, Face Centre, so its height runs along the ray.
		{
			Rig rig;
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			//-30 dB: the video, 1 - exp( -P ), is P to 0.05%, so a half-power
			//edge is the sprite's edge (at 0 dB the tube's response would
			//move it out by a fraction of the beam's blur).
			spriteScene( rig );
			rig.plugin.SetDecayForTest( false );
			rig.Set( PT_RPM, ParamFromRpm( 120.0 ) );
			rig.Set( PT_SPRITE_COUNT, 5.0f );
			rig.Set( PT_SPRITE_SIZE, ParamFromSpriteSize( 0.2 ) );
			rig.Set( PT_ORIENTATION, static_cast< float >( Orientation::FaceCentre ) );
			setSpriteFile( rig, square );
			if( !rig.Render( 31 ) )
				return 1;
			const double size = SpriteSizeFromParam( ParamFromSpriteSize( 0.2 ) );
			const Floats out  = rig.Output();
			const double cx = rig.plugin.ScopeCentreX(), cy = rig.plugin.ScopeCentreY(), R = rig.plugin.ScopeRadius();
			const auto sprites = rig.plugin.SpritesForTest();
			int measured = 0, wrong = 0;
			std::string what;
			for( size_t i = 0; i < sprites.size(); ++i )
			{
				const auto& s = sprites[ i ];
				if( !isolated( sprites, i ) )
					continue;
				++measured;
				const double ux = std::sin( s.bearing ), uy = std::cos( s.bearing );
				const double px = cx + s.x * R, py = cy + s.y * R;
				auto extent     = [ & ]( double dx, double dy ) {
					std::vector< double > profile;
					for( double d = -0.75 * size * R; d <= 0.75 * size * R; d += 0.05 )
						profile.push_back( screenLuminance( out, rig.width, rig.height, px + d * dx, py + d * dy ) );
					const double floor = *std::min_element( profile.begin(), profile.end() );
					for( double& v : profile )
						v -= floor;
					const Width w = halfMaxWidth( profile );
					return w.valid ? ( w.right - w.left ) * 0.05 : 0.0;
				};
				const double radial = extent( ux, uy ), across = extent( uy, -ux );
				const double polar  = radialExtent( rig, s, 0.75 * size );
				//One pixel on the screen (the bilinear read's tent and the
				//pixel grid's phase, twice); 1.5 bins on the phosphor.
				const bool ok = std::fabs( radial - size * R ) <= 1.0 && std::fabs( across - size * R ) <= 1.0
				                && std::fabs( polar - size ) <= 1.5 / kRanges;
				wrong += !ok;
				what += fmt( " R %.2f: %.2f x %.2f px, %.1f bins;", s.range, radial, across, polar * kRanges );
			}
			Check( measured >= 2 && wrong == 0,
			       fmt( "%dx%d  Sprite Size %.3f (%.2f px, %.1f bins), %d squares measured:%s", raster.w, raster.h, size, size * R,
			            size * kRanges, measured, what.c_str() ) );
		}

		//3. Orientation: the top-half sprite's light sits above its centre, on
		//either path. Upright: screen up. Face Centre: outward. Head In: inward.
		for( SpritePath path : { SpritePath::Closing, SpritePath::Transit } )
			for( Orientation orientation : { Orientation::Upright, Orientation::FaceCentre, Orientation::HeadIn } )
			{
				Rig rig;
				if( !rig.Init( raster.w, raster.h ) )
					return 1;
				spriteScene( rig );
				rig.plugin.SetDecayForTest( false );
				rig.plugin.SetSpriteOrientationIgnoredForTest( perturb.spriteOrientationIgnored );
				rig.plugin.SetSpriteHeadInAsFaceForTest( perturb.headInAsFace );
				rig.Set( PT_RPM, ParamFromRpm( 120.0 ) );
				rig.Set( PT_SPRITE_COUNT, 5.0f );
				rig.Set( PT_SPRITE_PATH, static_cast< float >( path ) );
				const double R    = 0.95 * 0.5 * std::min( raster.w, raster.h );
				const double want = std::max( 0.1, 18.0 / R );//at least 18 px across
				rig.Set( PT_SPRITE_SIZE, ParamFromSpriteSize( want ) );
				rig.Set( PT_ORIENTATION, static_cast< float >( orientation ) );
				setSpriteFile( rig, top );
				if( !rig.Render( 31 ) )
					return 1;
				const double size = SpriteSizeFromParam( ParamFromSpriteSize( want ) );
				const OrientationReading o = readOrientation( rig, size, orientation, 0.25 );
				//The pixel grid places each edge of the lit half to half a pixel,
				//so its centroid is good to half a pixel across and along the
				//offset: atan( 0.5 / offset ) and 0.5 / offset (offset in pixels).
				const double offset = 0.25 * size * R, angleBound = std::atan( 0.5 / offset ) * 180.0 / kPi;
				//Face Centre and Head In are only told from Upright by sprites away
				//from north (Head In from Face Centre by any).
				Check( o.measured >= 2 && o.offNorth >= 2 && o.wrong( angleBound, 0.5 / offset ) == 0,
				       fmt( "%dx%d  %s, %s: %d sprites, the light's centroid within %.2f deg of %s, at %.0f%%-%.0f%% of a quarter size (%.1f "
				            "px) from the centre (bounds %.1f deg, %.0f%%: half a pixel); %d of them more than 30 deg from north",
				            raster.w, raster.h, path == SpritePath::Transit ? "Transit" : "Closing", orientationName( orientation ),
				            o.measured, o.worstAngle, orientationWay( orientation ), 100.0 * ( 1.0 - o.worstLength ),
				            100.0 * ( 1.0 + o.worstLength ), offset, angleBound, 100.0 * 0.5 / offset, o.offNorth ) );
			}

		//3b. Head In with the asymmetric alien (tools/fixtures/alien.png): its
		//light sits above its middle by the centroid of its silhouette (the
		//head is wide at the top), and Head In turns that towards the centre.
		for( Orientation orientation : { Orientation::FaceCentre, Orientation::HeadIn } )
		{
			Rig rig;
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			spriteScene( rig );
			rig.plugin.SetDecayForTest( false );
			rig.plugin.SetSpriteOrientationIgnoredForTest( perturb.spriteOrientationIgnored );
			rig.plugin.SetSpriteHeadInAsFaceForTest( perturb.headInAsFace );
			rig.Set( PT_RPM, ParamFromRpm( 120.0 ) );
			rig.Set( PT_SPRITE_COUNT, 5.0f );
			const double R    = 0.95 * 0.5 * std::min( raster.w, raster.h );
			const double want = std::max( 0.15, 32.0 / R );//at least 32 px across
			rig.Set( PT_SPRITE_SIZE, ParamFromSpriteSize( want ) );
			rig.Set( PT_ORIENTATION, static_cast< float >( orientation ) );
			setSpriteFile( rig, fixturePath( "alien.png" ) );
			if( !rig.Render( 31 ) )
				return 1;
			const double size  = SpriteSizeFromParam( ParamFromSpriteSize( want ) );
			const double shift = silhouetteOffset( rig.plugin.SpriteMaskForTest(), 0.5 );
			const OrientationReading o = readOrientation( rig, size, orientation, shift );
			//The same half pixel, on an offset of only 6% of the sprite. A 320x180
			//scope has room for one isolated 32-pixel alien (the top-half square
			//above is the check that needs two).
			const double offset = shift * size * R, angleBound = std::atan( 0.5 / offset ) * 180.0 / kPi;
			Check( o.measured >= 1 && o.offNorth >= 1 && o.wrong( angleBound, 0.5 / offset ) == 0,
			       fmt( "%dx%d  the alien, %s: %d sprites, the light's centroid within %.2f deg of %s, at %.0f%%-%.0f%% of its "
			            "silhouette's offset (%.3f of its size, %.1f px) from the middle (bounds %.1f deg, %.0f%%: half a pixel)",
			            raster.w, raster.h, orientationName( orientation ), o.measured, o.worstAngle, orientationWay( orientation ),
			            100.0 * ( 1.0 - o.worstLength ), 100.0 * ( 1.0 + o.worstLength ), shift, offset, angleBound, 100.0 * 0.5 / offset ) );
		}

		//4. Grow On Approach: Size x ( 1 + 3 g ( 1 - R ) ), here g = 1.
		{
			Rig rig;
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			spriteScene( rig );
			rig.plugin.SetDecayForTest( false );
			rig.Set( PT_RPM, ParamFromRpm( 120.0 ) );
			rig.Set( PT_SPRITE_COUNT, 5.0f );
			rig.Set( PT_SPRITE_SIZE, ParamFromSpriteSize( 0.04 ) );
			rig.Set( PT_ORIENTATION, static_cast< float >( Orientation::FaceCentre ) );
			rig.Set( PT_SPRITE_GROW, 1.0f );
			setSpriteFile( rig, square );
			if( !rig.Render( 31 ) )
				return 1;
			const double size = SpriteSizeFromParam( ParamFromSpriteSize( 0.04 ) );
			int measured = 0, wrong = 0;
			std::string what;
			const auto sprites = rig.plugin.SpritesForTest();
			for( size_t i = 0; i < sprites.size(); ++i )
			{
				const auto& s      = sprites[ i ];
				const double grown = size * ( 1.0 + 3.0 * ( 1.0 - s.range ) );
				if( !isolated( sprites, i ) )
					continue;
				const double polar = radialExtent( rig, s, 0.75 * grown );
				++measured;
				wrong += std::fabs( polar - grown ) > 1.5 / kRanges;
				what += fmt( " R %.2f: %.4f (%.4f);", s.range, polar, grown );
			}
			Check( measured >= 3 && wrong == 0,
			       fmt( "%dx%d  Grow On Approach 1, Size %.3f: across, against Size x (1 + 3 (1 - R)) to 1.5 bins:%s", raster.w,
			            raster.h, size, what.c_str() ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --sprite-speed: from the painted frames, a sprite closes at Sprite Speed.
//===========================================================================
int runSpriteSpeed( const Perturb& perturb )
{
	std::printf( "\n=== sprite-speed: read off the phosphor each time the beam paints it, a sprite closes on the centre at Sprite "
	             "Speed and starts again at the rim\n" );
	const double v      = SpriteSpeedFromParam( ParamFromSpriteSpeed( 0.15 ) );
	const double pulse  = kMetresPerMicrosecond * PulseFromParam( ParamFromPulse( 0.05 ) ) / 1000.0 / 24.0;
	const double r0     = 1.0 - swarm::Swarm::StartOf( 0 ), tStar = r0 / v;
	for( const Raster& raster : kRasters )
		for( double fps : { 60.0, 24.0 } )
		{
			Rig rig;
			rig.fps = fps;
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			spriteScene( rig );
			rig.plugin.SetSpritePerFrameForTest( perturb.spriteSpeedPerFrame );
			rig.Set( PT_RPM, ParamFromRpm( 120.0 ) );
			rig.Set( PT_PERSISTENCE, ParamFromPersistence( 0.1 ) );
			rig.Set( PT_SPRITE_COUNT, 1.0f );
			rig.Set( PT_SPRITE_SIZE, ParamFromSpriteSize( 0.05 ) );
			rig.Set( PT_SPRITE_SPEED, ParamFromSpriteSpeed( 0.15 ) );
			struct Sample
			{
				double t, r;
				uint32_t generation;
			};
			std::vector< Sample > samples;
			double tRespawn = -1.0;
			for( int f = 0; f < static_cast< int >( 6.2 * fps ); ++f )
			{
				const double b0 = rig.plugin.AntennaBins();
				if( !rig.Render( 1 ) )
					return 1;
				const double t = f / fps;
				if( tRespawn < 0.0 && t >= tStar - 1e-9 )
					tRespawn = t;
				const double b1 = rig.plugin.AntennaBins();
				const auto s    = rig.plugin.SpritesForTest()[ 0 ];
				const int col   = nearestColumn( s.bearing );
				bool crossed    = false;
				for( int64_t k = sweep::floorToInt( b0 ) + 1; k <= sweep::floorToInt( b1 ); ++k )
					crossed = crossed || sweep::Column( k, kBearings ) == col;
				if( !crossed )
					continue;
				//The flash share: what this paint put there, the last one's
				//gone by e^-14 (35 ms against a rotation of 0.5 s).
				const Floats column = rig.PhosphorRect4( col, 0, 1, kRanges );
				double sum = 0.0, moment = 0.0;
				for( int i = 0; i < kRanges; ++i )
				{
					const double value = column[ static_cast< size_t >( i ) * 4 + 2 ];
					sum += value;
					moment += value * ( i + 0.5 ) / kRanges;
				}
				//A disc the rim or the centre cuts is not a disc: its centroid
				//is not its centre. The blip at Threshold 0.5 is Size / 2 across.
				const double want   = tRespawn < 0.0 ? r0 - v * t : 1.0 - v * ( t - tRespawn );
				const double radius = 0.25 * SpriteSizeFromParam( ParamFromSpriteSize( 0.05 ) );
				if( sum > 0.0 && want - radius > 2.0 / kRanges && want + radius + pulse < 1.0 - 2.0 / kRanges )
					samples.push_back( { t, moment / sum - 0.5 * pulse, tRespawn >= 0.0 ? 1u : 0u } );
			}
			//The law, from the stated speed alone: r0 - v t, then 1 - v ( t - t_r ).
			double worst = 0.0, sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
			int before = 0, after = 0;
			for( const Sample& s : samples )
			{
				const double want = s.generation == 0 ? r0 - v * s.t : 1.0 - v * ( s.t - tRespawn );
				worst             = std::max( worst, std::fabs( s.r - want ) );
				if( s.generation == 0 )
				{
					++before;
					sx += s.t;
					sy += s.r;
					sxx += s.t * s.t;
					sxy += s.t * s.r;
				}
				else
					++after;
			}
			const double slope = before > 1 ? ( before * sxy - sx * sy ) / ( before * sxx - sx * sx ) : 0.0;
			const double zero  = slope != 0.0 ? -( sy - slope * sx ) / before / slope : 0.0;
			//A quarter of a range bin per paint: the centroid of a disc's
			//chord, sampled at bin centres, its two-tap edges symmetric.
			const double bound = 0.25 / kRanges;
			Check( before >= 6 && after >= 1 && worst <= bound && std::fabs( slope + v ) <= 1e-3 * v && std::fabs( zero - tStar ) <= 1.0 / fps,
			       fmt( "%dx%d %2.0f fps: %d paints closing, %d after the rim: off the law by at most %.2f bins (bound 0.25); slope "
			            "%.5f radii/s against %.5f; reaches the centre at %.3f s (t* %.3f)",
			            raster.w, raster.h, fps, before, after, worst * kRanges, -slope, v, zero, tStar ) );
		}
	return Verdict();
}

//===========================================================================
// --sprite-beam: the beam reveals a sprite and the phosphor lets it go.
//===========================================================================
int runSpriteBeam( const Perturb& perturb )
{
	std::printf( "\n=== sprite-beam: a sprite is dark until the sweep reaches it, then its share decays as A_f e^(-t/tau_f) + A_a "
	             "e^(-t/tau_a) until the next sweep\n" );
	for( const Raster& raster : kRasters )
	{
		Rig rig;
		if( !rig.Init( raster.w, raster.h ) )
			return 1;
		spriteScene( rig );
		rig.plugin.SetSpriteNoFadeForTest( perturb.spriteNoFade );
		const double rpm = 20.0, tauA = 1.5, flash = 2.0;
		rig.Set( PT_RPM, ParamFromRpm( rpm ) );
		rig.Set( PT_PERSISTENCE, ParamFromPersistence( tauA ) );
		rig.Set( PT_FLASH, static_cast< float >( flash / 4.0 ) );
		rig.Set( PT_PHOSPHOR, static_cast< float >( Phosphor::P7 ) );
		rig.Set( PT_SPRITE_COUNT, 1.0f );
		rig.Set( PT_SPRITE_SIZE, ParamFromSpriteSize( 0.1 ) );
		if( !rig.Render( 1 ) )
			return 1;
		const auto sp       = rig.plugin.SpritesForTest()[ 0 ];
		const int column    = nearestColumn( sp.bearing );
		const int row       = static_cast< int >( sp.range * kRanges );
		const double tauF   = FlashTauFor( Phosphor::P7 );
		const double aFlash = FlashFromParam( static_cast< float >( flash / 4.0 ) );
		rig.plugin.SetPolarViewForTest( true, column - 8, row - 8, true );
		const double rate   = RpmFromParam( ParamFromRpm( rpm ) ) / 60.0 * kBearings;
		const double paint1 = ( column + 0.5 ) / rate, period = kBearings / rate;
		double energy = -1.0, worstF = 0.0, worstA = 0.0, repaintFlash = 0.0, shareOff = 0.0;
		int dark = 0, lit = 0, wrongF = 0, wrongA = 0, frames = 0;
		for( int f = 1; f < static_cast< int >( ( paint1 + 1.6 * period ) * 60.0 ); ++f )
		{
			if( !rig.Render( 1 ) )
				return 1;
			const double t = f / 60.0;
			Floats texel( 4 );
			glBindFramebuffer( GL_FRAMEBUFFER, rig.outputFBO );
			glReadPixels( 8, 8, 1, 1, GL_RGBA, GL_FLOAT, texel.data() );
			if( t < paint1 - 1.0 / 60.0 )
			{
				//Before the sweep: the sprite is there, nothing shows it.
				const Floats raw = rig.PhosphorRect4( column, row, 1, 1 );
				dark += texel[ 0 ] != 0.0f || texel[ 1 ] != 0.0f || raw[ 0 ] != 0.0f || raw[ 1 ] != 0.0f;
				++lit;
				continue;
			}
			if( t < paint1 )
				continue;
			double wantF = 0.0, wantA = 0.0;
			for( double p = paint1; p <= t; p += period )
			{
				wantF += aFlash * std::exp( -( t - p ) / tauF );
				wantA += std::exp( -( t - p ) / tauA );
			}
			if( energy < 0.0 )
			{
				energy = texel[ 1 ] / wantA;
				//In a scene of nothing but the sprite, the share is the whole.
				const Floats raw = rig.PhosphorRect4( column, row, 1, 1 );
				shareOff         = std::max( std::fabs( raw[ 2 ] - raw[ 0 ] ) / raw[ 0 ], std::fabs( raw[ 3 ] - raw[ 1 ] ) / raw[ 1 ] );
			}
			const double tol = 8.0 * kUlp;
			const double eF  = std::fabs( texel[ 0 ] - energy * wantF ) - 1e-12 * energy;
			const double eA  = std::fabs( texel[ 1 ] - energy * wantA ) - 1e-12 * energy;
			worstF           = std::max( worstF, eF / ( energy * std::max( wantF, 1e-300 ) ) );
			worstA           = std::max( worstA, eA / ( energy * wantA ) );
			wrongF += eF > tol * energy * wantF;
			wrongA += eA > tol * energy * wantA;
			if( t >= paint1 + period && repaintFlash == 0.0 )
				repaintFlash = texel[ 0 ] / ( energy * aFlash * std::exp( -( t - paint1 - period ) / tauF ) );
			++frames;
		}
		Check( lit > 10 && dark == 0, fmt( "%dx%d  %d frames before the sweep first reaches the sprite (%.2f s): its texels are 0, "
		                                   "%d were not",
		                                   raster.w, raster.h, lit, paint1, dark ) );
		Check( wrongF == 0 && wrongA == 0 && energy > 0.0 && shareOff <= 4.0 * kUlp,
		       fmt( "%dx%d  %d frames after: the sprites' share decays as the law to %.2e (flash) and %.2e (afterglow), bound 8 ulp; "
		            "with nothing else lit it is the whole echo to %.1e (bound 4 ulp)",
		            raster.w, raster.h, frames, worstF, worstA, shareOff ) );
		Check( std::fabs( repaintFlash - 1.0 ) <= 16.0 * kUlp,
		       fmt( "%dx%d  60/RPM = %.2f s later the sweep repaints it: its flash back to %.6f of the first (bound 16 ulp)", raster.w,
		            raster.h, period, repaintFlash ) );
	}
	return Verdict();
}

//===========================================================================
// --sprite-repeat: the same settings and clock, the same picture.
//===========================================================================
int runSpriteRepeat( const Perturb& perturb )
{
	std::printf( "\n=== sprite-repeat: two instances with the same settings and clock render the same frame, to the bit, and a "
	             "frame's sprites are where the clock says at any frame rate\n" );
	auto configure = [ & ]( Rig& rig ) {
		//Clutter and noise stay (their speckle is hashed too); the land and
		//the rain go, because the coast's shadow costs the software renderer
		//a third of a second a frame and has nothing to do with sprites.
		rig.Set( PT_LAND, 0.0f );
		rig.Set( PT_RAIN, 0.0f );
		rig.plugin.SetSpriteWallSeedForTest( perturb.spriteWallSeed );
		rig.Set( PT_SPRITE_COUNT, 12.0f );
		rig.Set( PT_SPRITE_SPEED, ParamFromSpriteSpeed( 0.05 ) );
		rig.Set( PT_SPEED_SPREAD, 0.6f );
		rig.Set( PT_STABILITY, 0.3f );
		rig.Set( PT_ORIENTATION, static_cast< float >( Orientation::FaceCentre ) );
		rig.Set( PT_SPRITE_GROW, 0.5f );
		rig.Set( PT_SPRITE_HUE, 0.3f );
		rig.Set( PT_SPRITE_SATURATION, 0.8f );
		setSpriteFile( rig, fixturePath( "alien.png" ) );
	};
	for( const Raster& raster : kRasters )
	{
		Rig a, b, c;
		c.fps = 30.0;
		if( !a.Init( raster.w, raster.h ) || !b.Init( raster.w, raster.h ) || !c.Init( raster.w, raster.h ) )
			return 1;
		configure( a );
		configure( b );
		configure( c );
		if( !a.Render( 121 ) || !b.Render( 121 ) || !c.Render( 61 ) )
			return 1;
		const Floats pa = a.Output(), pb = b.Output();
		size_t differ   = 0;
		for( size_t i = 0; i < pa.size(); ++i )
			differ += pa[ i ] != pb[ i ];
		const auto sa = a.plugin.SpritesForTest(), sc = c.plugin.SpritesForTest();
		double worst  = 0.0;
		int compared  = 0;
		for( size_t i = 0; i < sa.size() && i < sc.size(); ++i )
		{
			//Only a sprite on its first approach in both runs: one that has
			//been back to the rim dropped a frame-sized overshoot there.
			if( sa[ i ].generation != 0 || sc[ i ].generation != 0 )
				continue;
			++compared;
			worst = std::max( { worst, std::fabs( sa[ i ].x - sc[ i ].x ), std::fabs( sa[ i ].y - sc[ i ].y ) } );
		}
		Check( differ == 0, fmt( "%dx%d  2 s of 12 alien sprites, wandering, Face Centre, growing, tinted, over clutter and noise: %zu of %zu output floats "
		                         "differ between the two instances",
		                         raster.w, raster.h, differ, pa.size() ) );
		Check( compared >= 6 && worst < 1e-9,
		       fmt( "%dx%d  the same 2 s at 30 fps: %d sprites still on their first approach where 60 fps put them, to %.1e", raster.w,
		            raster.h, compared, worst ) );
	}
	return Verdict();
}

//===========================================================================
// --sprite-colour: Hue turns the phosphor's colours, Saturation scales them.
//===========================================================================
int runSpriteColour( const Perturb& perturb )
{
	std::printf( "\n=== sprite-colour: the sprites glow in the phosphor's colours turned by Hue about the grey axis, their chroma "
	             "scaled by 2 x Saturation\n" );
	const std::string square = scratchFolder() + "/square.png";
	writeAlphaSprite( square, 16, 16, []( int, int ) { return 1.0; } );
	for( const Raster& raster : kRasters )
	{
		struct Setting
		{
			double hue, saturation;
		};
		const Setting settings[] = { { 0.0, 0.5 }, { 1.0 / 3.0, 0.5 }, { 0.0, 0.75 }, { 0.0, 0.0 } };
		Floats out[ 5 ];
		for( int k = 0; k < 5; ++k )
		{
			Rig rig;
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			spriteScene( rig, 0.0 );
			rig.plugin.SetSpriteHueBackwardsForTest( perturb.spriteHueBackwards );
			rig.Set( PT_RPM, ParamFromRpm( 120.0 ) );
			//The fifth: no sprites at all, the empty face to take away.
			rig.Set( PT_SPRITE_COUNT, k == 4 ? 0.0f : 5.0f );
			rig.Set( PT_SPRITE_SIZE, ParamFromSpriteSize( 0.15 ) );
			rig.Set( PT_SPRITE_HUE, static_cast< float >( settings[ k % 4 ].hue ) );
			rig.Set( PT_SPRITE_SATURATION, static_cast< float >( settings[ k % 4 ].saturation ) );
			setSpriteFile( rig, square );
			if( !rig.Render( 40 ) )
				return 1;
			out[ k ] = rig.Output();
		}
		//Light before the tube: -ln( 1 - out ), less the same pixel's with no
		//sprites (the face, and the scope's edge).
		auto light = [ & ]( const Floats& o, size_t i, double l[ 3 ] ) {
			for( int c = 0; c < 3; ++c )
				l[ c ] = -std::log( 1.0 - static_cast< double >( o[ i * 4 + c ] ) ) + std::log( 1.0 - static_cast< double >( out[ 4 ][ i * 4 + c ] ) );
		};
		const double a = 2.0 * kPi / 3.0, k3 = 1.0 / std::sqrt( 3.0 );
		double worst[ 3 ] = { 0.0, 0.0, 0.0 };
		int pixels        = 0;
		for( size_t i = 0; i < static_cast< size_t >( raster.w ) * raster.h; ++i )
		{
			double base[ 3 ];
			light( out[ 0 ], i, base );
			const double scale = std::max( { base[ 0 ], base[ 1 ], base[ 2 ] } );
			if( scale < 0.2 )
				continue;
			++pixels;
			const double mean = ( base[ 0 ] + base[ 1 ] + base[ 2 ] ) / 3.0;
			const double cross[ 3 ] = { k3 * ( base[ 2 ] - base[ 1 ] ), k3 * ( base[ 0 ] - base[ 2 ] ), k3 * ( base[ 1 ] - base[ 0 ] ) };
			double l[ 3 ];
			for( int s = 1; s < 4; ++s )
			{
				light( out[ s ], i, l );
				for( int c = 0; c < 3; ++c )
				{
					double want;
					if( s == 1 )
						want = base[ c ] * std::cos( a ) + cross[ c ] * std::sin( a ) + mean * ( 1.0 - std::cos( a ) );
					else
						want = mean + ( base[ c ] - mean ) * 2.0 * settings[ s ].saturation;
					worst[ s - 1 ] = std::max( worst[ s - 1 ], std::fabs( l[ c ] - want ) / scale );
				}
			}
		}
		//1e-4 of the brightest channel: float -ln( 1 - out ) near out = 0.9
		//is good to ~1e-6 relative, and the share is the whole echo to ulps.
		Check( pixels > 50 && worst[ 0 ] < 1e-4 && worst[ 1 ] < 1e-4 && worst[ 2 ] < 1e-4,
		       fmt( "%dx%d  %d sprite pixels, P7: Hue 1/3 is the untinted light turned 120 deg (off by %.1e), Saturation 0.75 its "
		            "chroma x1.5 (%.1e), Saturation 0 grey (%.1e); bound 1e-4",
		            raster.w, raster.h, pixels, worst[ 0 ], worst[ 1 ], worst[ 2 ] ) );

		//Sprite Colour (since v0.2.0). Picture with the alien: every sprite
		//pixel's light has the picture's chromaticity, (115, 242, 89) / 255;
		//Hue 1/3 turns that light 120 degrees. Phosphor with the alien: the
		//phosphor's own two colours, and the share untouched (the fast path).
		//Picture with no colour to give (the blip, the grey panda) is Phosphor,
		//to the bit.
		struct Case
		{
			SpriteColour colour;
			std::string file;
			double hue;
			bool tint;
		};
		const std::string alien = fixturePath( "alien.png" ), panda = fixturePath( "panda.png" );
		const Case cases[] = { { SpriteColour::Picture, alien, 0.0, false },  { SpriteColour::Picture, alien, 1.0 / 3.0, false },
			                   { SpriteColour::Phosphor, alien, 0.0, false }, { SpriteColour::Picture, "", 0.0, false },
			                   { SpriteColour::Phosphor, "", 0.0, false },    { SpriteColour::Picture, panda, 0.0, false },
			                   { SpriteColour::Phosphor, panda, 0.0, false } };
		Floats pic[ 7 ];
		bool tinted[ 7 ];
		float flashColour[ 3 ] = {}, afterColour[ 3 ] = {};
		for( int k = 0; k < 7; ++k )
		{
			Rig rig;
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			spriteScene( rig, 0.0 );
			rig.plugin.SetSpriteColourSwappedForTest( perturb.colourSwapped );
			rig.Set( PT_RPM, ParamFromRpm( 120.0 ) );
			rig.Set( PT_SPRITE_COUNT, 5.0f );
			rig.Set( PT_SPRITE_SIZE, ParamFromSpriteSize( 0.3 ) );
			rig.Set( PT_SPRITE_COLOUR, static_cast< float >( cases[ k ].colour ) );
			rig.Set( PT_SPRITE_HUE, static_cast< float >( cases[ k ].hue ) );
			setSpriteFile( rig, cases[ k ].file );
			if( !rig.Render( 40 ) )
				return 1;
			pic[ k ]    = rig.Output();
			tinted[ k ] = rig.plugin.SpriteTintForTest();
			if( k == 2 )
			{
				std::copy( rig.plugin.SpriteFlashColourForTest(), rig.plugin.SpriteFlashColourForTest() + 3, flashColour );
				std::copy( rig.plugin.SpriteAfterColourForTest(), rig.plugin.SpriteAfterColourForTest() + 3, afterColour );
			}
		}
		const double alienRgb[ 3 ] = { 115.0 / 255.0, 242.0 / 255.0, 89.0 / 255.0 };
		const double alienSum      = alienRgb[ 0 ] + alienRgb[ 1 ] + alienRgb[ 2 ];
		double chroma = 0.0, turned = 0.0, phosphorFit = 0.0, pictureFit = 0.0;
		int lit       = 0;
		auto fitOf    = [ & ]( const double l[ 3 ] ) {
			//Least squares of l against a F + b A (the phosphor's flash and
			//afterglow colours): what is left over.
			double ff = 0, fa = 0, aa = 0, fl = 0, al = 0;
			for( int c = 0; c < 3; ++c )
			{
				ff += flashColour[ c ] * flashColour[ c ];
				fa += flashColour[ c ] * afterColour[ c ];
				aa += afterColour[ c ] * afterColour[ c ];
				fl += flashColour[ c ] * l[ c ];
				al += afterColour[ c ] * l[ c ];
			}
			const double det = ff * aa - fa * fa, a = ( fl * aa - al * fa ) / det, b = ( al * ff - fl * fa ) / det;
			double rest      = 0.0;
			for( int c = 0; c < 3; ++c )
				rest = std::max( rest, std::fabs( l[ c ] - a * flashColour[ c ] - b * afterColour[ c ] ) );
			return rest;
		};
		const double ca = std::cos( 2.0 * kPi / 3.0 ), sa = std::sin( 2.0 * kPi / 3.0 );
		for( size_t i = 0; i < static_cast< size_t >( raster.w ) * raster.h; ++i )
		{
			double l[ 3 ], h[ 3 ], ph[ 3 ];
			light( pic[ 0 ], i, l );
			light( pic[ 1 ], i, h );
			light( pic[ 2 ], i, ph );
			const double scale = std::max( { l[ 0 ], l[ 1 ], l[ 2 ] } );
			if( scale < 0.2 || std::max( { ph[ 0 ], ph[ 1 ], ph[ 2 ] } ) < 0.2 )
				continue;
			++lit;
			const double sum = l[ 0 ] + l[ 1 ] + l[ 2 ];
			const double mean = sum / 3.0;
			const double cross[ 3 ] = { k3 * ( l[ 2 ] - l[ 1 ] ), k3 * ( l[ 0 ] - l[ 2 ] ), k3 * ( l[ 1 ] - l[ 0 ] ) };
			for( int c = 0; c < 3; ++c )
			{
				chroma = std::max( chroma, std::fabs( l[ c ] / sum - alienRgb[ c ] / alienSum ) );
				turned = std::max( turned, std::fabs( h[ c ] - ( l[ c ] * ca + cross[ c ] * sa + mean * ( 1.0 - ca ) ) ) / scale );
			}
			phosphorFit = std::max( phosphorFit, fitOf( ph ) / std::max( { ph[ 0 ], ph[ 1 ], ph[ 2 ] } ) );
			pictureFit  = std::max( pictureFit, fitOf( l ) / scale );
		}
		Check( lit > 50 && chroma < 1e-4 && turned < 1e-4,
		       fmt( "%dx%d  Picture, the alien: %d sprite pixels glow its colour, (115, 242, 89) / 255, to %.1e in chromaticity; Hue 1/3 "
		            "turns that light 120 deg (off by %.1e); bound 1e-4",
		            raster.w, raster.h, lit, chroma, turned ) );
		Check( !tinted[ 2 ] && phosphorFit < 1e-4 && pictureFit > 1e-2 && tinted[ 0 ],
		       fmt( "%dx%d  Phosphor, the alien: the share is left alone (%s) and every pixel is the phosphor's flash and afterglow to "
		            "%.1e; Picture's is not (%.1e off them)",
		            raster.w, raster.h, tinted[ 2 ] ? "NO" : "yes", phosphorFit, pictureFit ) );
		Check( pic[ 3 ] == pic[ 4 ] && pic[ 5 ] == pic[ 6 ] && !tinted[ 3 ] && !tinted[ 5 ],
		       fmt( "%dx%d  Picture with the blip, and with the grey panda, draws exactly what Phosphor does, the share untouched: %s, %s",
		            raster.w, raster.h, pic[ 3 ] == pic[ 4 ] ? "yes" : "NO", pic[ 5 ] == pic[ 6 ] ? "yes" : "NO" ) );
	}
	return Verdict();
}

//===========================================================================
// --sprite-path (no GL): Transit's law and the latecomers' schedule, on the
// CPU, in double. The harness states each law itself and holds Swarm.cpp to
// it; the GL checks then hold the painted picture to the same laws.
//===========================================================================
/// The harness's own statement of the latecomers' window, scope radii at
/// Sprite Speed: 85% of the shortest track (one radius; Transit's chord at
/// |x| = 0.85) over the fastest a sprite can go, 3^Spread x ( 1 + 0.9 ( 1 - Stability ) ).
double arrivalWindowLaw( swarm::Path path, double spread, double stability )
{
	const double shortest = path == swarm::Path::Transit ? 2.0 * std::sqrt( 1.0 - 0.85 * 0.85 ) : 1.0;
	return 0.85 * shortest / ( std::pow( 3.0, spread ) * ( 1.0 + 0.9 * ( 1.0 - stability ) ) );
}

int runSpritePath( const Perturb& perturb )
{
	std::printf( "\n=== sprite-path: Transit runs straight down a chord from the top rim to the bottom rim at Sprite Speed, entered "
	             "uniformly across the middle 85%%; latecomers come in on their schedule; both the same at any frame rate\n" );
	using swarm::Path;
	using swarm::Settings;
	using swarm::Swarm;
	auto configure = [ & ]( Swarm& sw ) {
		sw.SetEntryWideForTest( perturb.transitEntryWide );
		sw.SetArrivalNaiveForTest( perturb.arrivalNaive );
		sw.SetTransitNorthForTest( perturb.transitNorth );
	};
	struct Clock
	{
		const char* name;
		std::vector< double > t;
	};

	//1. Steady: x stays where the track came in; y falls at v from where the
	//generation started (the first StartOf( 0 ) of its chord in, every later
	//one at the top rim, sqrt( 1 - x^2 )); it ends at the bottom rim, the
	//first frame at or after y would pass -sqrt( 1 - x^2 ).
	{
		const double v = 0.5;
		Settings s;
		s.count     = 1;
		s.seed      = 11;
		s.speed     = v;
		s.stability = 1.0;
		s.path      = Path::Transit;
		const std::vector< Clock > clocks = { { "60 fps", frameTimes( 60.0, 24.0, false ) },
			                                  { "24 fps", frameTimes( 24.0, 24.0, false ) },
			                                  { "59.94 fps", frameTimes( 59.94, 24.0, false ) },
			                                  { "jittered 8-30 ms", frameTimes( 0.0, 24.0, true ) } };
		for( const Clock& clock : clocks )
		{
			Swarm sw;
			configure( sw );
			double worst = 0.0, widest = 0.0, farthest = 0.0;
			int generations = 0, late = 0;
			uint32_t generation = 0;
			double x0 = 0.0, half = 0.0, ys = 0.0, ts = 0.0;
			for( size_t i = 0; i < clock.t.size(); ++i )
			{
				const double dt = i == 0 ? 0.0 : clock.t[ i ] - clock.t[ i - 1 ];
				sw.Step( dt, s );
				const swarm::Placed p = sw.Sprites()[ 0 ];
				if( i == 0 || p.generation != generation )
				{
					if( i > 0 )
					{
						const double tStar = ts + ( ys + half ) / v;
						late += !( clock.t[ i ] >= tStar - 1e-9 && clock.t[ i - 1 ] < tStar + 1e-9 );
						++generations;
					}
					generation = p.generation;
					x0         = p.x;
					half       = std::sqrt( 1.0 - x0 * x0 );
					ys         = i == 0 ? half - Swarm::StartOf( 0 ) * 2.0 * half : half;
					ts         = clock.t[ i ];
					widest     = std::max( widest, std::fabs( x0 ) );
				}
				worst    = std::max( { worst, std::fabs( p.x - x0 ), std::fabs( p.y - ( ys - v * ( clock.t[ i ] - ts ) ) ) } );
				farthest = std::max( farthest, std::hypot( p.x, p.y ) );
			}
			Check( generations >= 4 && worst < 1e-9 && late == 0 && widest <= 0.85 && farthest <= 1.0 + 1e-12,
			       fmt( "%-16s %d tracks: x held and y = y_start - v t to %.1e (bound 1e-9); every exit at the bottom rim within a "
			            "frame (%d late), every entry at the top rim; |x| <= %.4f (band 0.85); never past r = %.15f",
			            clock.name, generations, worst, late, widest, farthest ) );
		}
	}

	//2. Where they come in: uniform across |x| <= 0.85. Every track 32
	//sprites start in five minutes, against that by Kolmogorov-Smirnov.
	{
		Settings s;
		s.count     = 32;
		s.seed      = 11;
		s.speed     = 0.5;
		s.spread    = 1.0;
		s.stability = 1.0;
		s.path      = Path::Transit;
		Swarm sw;
		configure( sw );
		std::vector< double > entries;
		std::vector< uint32_t > seen( 32, 0xffffffffu );
		for( int f = 0; f <= 300 * 30; ++f )
		{
			sw.Step( f == 0 ? 0.0 : 1.0 / 30.0, s );
			for( const swarm::Placed& p : sw.Sprites() )
				if( seen[ p.slot ] != p.generation )
				{
					seen[ p.slot ] = p.generation;
					entries.push_back( p.x );
				}
		}
		std::sort( entries.begin(), entries.end() );
		const double n = static_cast< double >( entries.size() );
		double ks      = 0.0;
		for( size_t i = 0; i < entries.size(); ++i )
		{
			const double F = std::clamp( ( entries[ i ] + 0.85 ) / 1.7, 0.0, 1.0 );
			ks             = std::max( { ks, ( i + 1 ) / n - F, F - i / n } );
		}
		const double widest = std::max( -entries.front(), entries.back() );
		//1.63 / sqrt( n ): Kolmogorov-Smirnov's 1% critical value.
		Check( n >= 1000 && ks < 1.63 / std::sqrt( n ) && widest <= 0.85 && widest > 0.84,
		       fmt( "%.0f tracks: x from %.4f to %.4f, uniform over [-0.85, 0.85] by Kolmogorov-Smirnov (D %.4f, 1%% critical %.4f); the "
		            "shortest track %.3f radii",
		            n, entries.front(), entries.back(), ks, 1.63 / std::sqrt( n ), 2.0 * std::sqrt( std::max( 0.0, 1.0 - widest * widest ) ) ) );
	}

	//3. Stability 0: the weave is across the track and only across it -- x
	//leaves x0 by at most 0.15 x 4 f ( 1 - f ), y is the track's -- so the
	//sprite stays inside the scope and still only goes south; a sprite that
	//is held does not sway; and 24, 60, 240 fps and a jittered clock agree.
	{
		Settings s;
		s.count     = 8;
		s.seed      = 11;
		s.speed     = 0.08;
		s.spread    = 0.5;
		s.stability = 0.0;
		s.path      = Path::Transit;
		Swarm sw;
		configure( sw );
		double over = -1.0, swing = 0.0, along = 0.0, farthest = 0.0;
		int north = 0, samples = 0;
		std::vector< double > lastY( 8, 2.0 );
		std::vector< uint32_t > lastGen( 8, 0xffffffffu );
		for( int f = 0; f <= 40 * 60; ++f )
		{
			sw.Step( f == 0 ? 0.0 : 1.0 / 60.0, s );
			for( const swarm::Placed& p : sw.Sprites() )
			{
				const double x0   = Swarm::EntryOf( s.seed, p.slot, p.generation );
				const double half = std::sqrt( 1.0 - x0 * x0 );
				const double dev  = p.x - x0;
				over              = std::max( over, std::fabs( dev ) - 0.15 * 4.0 * p.progress * ( 1.0 - p.progress ) );
				swing             = std::max( swing, std::fabs( dev ) );
				along             = std::max( along, std::fabs( p.y - ( half - p.progress * 2.0 * half ) ) );
				farthest          = std::max( farthest, std::hypot( p.x, p.y ) );
				if( lastGen[ p.slot ] == p.generation )
					north += p.y >= lastY[ p.slot ];
				lastGen[ p.slot ] = p.generation;
				lastY[ p.slot ]   = p.y;
				++samples;
			}
		}
		Check( over <= 1e-12 && swing > 0.03 && along < 1e-12 && farthest <= 1.0 + 1e-12 && north == 0,
		       fmt( "Stability 0, 8 sprites for 40 s (%d samples): sideways by up to %.4f radii, never past 0.15 x 4 f (1 - f) (by %.1e); "
		            "along the track exactly where y says (%.1e); never past r = %.15f; %d steps north",
		            samples, swing, std::max( over, 0.0 ), along, farthest, north ) );

		const double T              = 6.0;
		std::vector< Clock > clocks = { { "60 fps", frameTimes( 60.0, T, false ) },
			                            { "24 fps", frameTimes( 24.0, T, false ) },
			                            { "240 fps", frameTimes( 240.0, T, false ) },
			                            { "jittered", frameTimes( 0.0, T - 0.031, true ) } };
		clocks.back().t.push_back( T );
		std::vector< std::vector< swarm::Placed > > ends;
		for( const Clock& clock : clocks )
		{
			Swarm c;
			configure( c );
			for( size_t i = 0; i < clock.t.size(); ++i )
				c.Step( i == 0 ? 0.0 : clock.t[ i ] - clock.t[ i - 1 ], s );
			ends.push_back( c.Sprites() );
		}
		double worst = 0.0;
		int compared = 0;
		for( size_t k = 0; k < ends[ 0 ].size(); ++k )
		{
			bool still = true;
			for( const auto& e : ends )
				still = still && e.size() == ends[ 0 ].size() && e[ k ].generation == 0;
			if( !still )
				continue;
			++compared;
			for( const auto& e : ends )
				worst = std::max( { worst, std::fabs( e[ k ].x - ends[ 0 ][ k ].x ), std::fabs( e[ k ].y - ends[ 0 ][ k ].y ) } );
		}
		Swarm held;
		configure( held );
		Settings still = s;
		still.speed    = 0.0;
		held.Step( 0.0, still );
		const auto before = held.Sprites();
		for( int f = 0; f < 300; ++f )
			held.Step( 1.0 / 60.0, still );
		const auto after = held.Sprites();
		bool unmoved     = before.size() == after.size();
		for( size_t k = 0; unmoved && k < before.size(); ++k )
			unmoved = before[ k ].x == after[ k ].x && before[ k ].y == after[ k ].y;
		Check( compared >= 4 && worst < 1e-9 && unmoved,
		       fmt( "Stability 0 after %.0f s at 60, 24, 240 fps and jittered: %d sprites on their first track agree to %.1e (bound "
		            "1e-9); at Sprite Speed 0 they hold still for 5 s without swaying: %s",
		            T, compared, worst, unmoved ? "yes" : "NO" ) );
	}

	//4. Latecomers. Start With 3 of 12, steady: the j-th of the 9 latecomers
	//comes in at the rim (the top rim on Transit) at T = wait / v, its wait in
	//the j-th ninth of the window W; from there it runs its track at v. The
	//harness states W and the ninths.
	for( Path path : { Path::Closing, Path::Transit } )
	{
		const double v = 0.2;
		Settings s;
		s.count     = 12;
		s.seed      = 11;
		s.speed     = v;
		s.stability = 1.0;
		s.path      = path;
		s.startWith = 3;
		const double W = arrivalWindowLaw( path, 0.0, 1.0 );
		const std::vector< Clock > clocks = { { "60 fps", frameTimes( 60.0, 6.0, false ) },
			                                  { "24 fps", frameTimes( 24.0, 6.0, false ) },
			                                  { "jittered", frameTimes( 0.0, 6.0, true ) } };
		for( const Clock& clock : clocks )
		{
			Swarm sw;
			configure( sw );
			sw.Step( 0.0, s );
			const bool startOk = sw.Sprites().size() == 3 && sw.Waiting() == 9;
			std::vector< double > arrive( 12, -1.0 );
			double inWindow = 0.0, worst = 0.0;
			int outOfPart   = 0;
			for( uint32_t i = 3; i < 12; ++i )
			{
				const double share = Swarm::WaitOf( s, i, static_cast< int >( i ) - 3, 9 ) / W;
				inWindow           = std::max( inWindow, share );
				outOfPart += !( share >= ( i - 3 ) / 9.0 && share < ( i - 2 ) / 9.0 );
			}
			int lateIn = 0;
			for( size_t f = 1; f < clock.t.size(); ++f )
			{
				sw.Step( clock.t[ f ] - clock.t[ f - 1 ], s );
				for( const swarm::Placed& p : sw.Sprites() )
				{
					if( p.slot < 3 || p.generation != 0 )
						continue;
					const double T = Swarm::WaitOf( s, p.slot, static_cast< int >( p.slot ) - 3, 9 ) / v;
					if( arrive[ p.slot ] < 0.0 )
					{
						arrive[ p.slot ] = clock.t[ f ];
						lateIn += !( clock.t[ f ] >= T - 1e-9 && clock.t[ f - 1 ] < T + 1e-9 );
					}
					//Where its track says it is, v ( t - T ) along from the rim.
					const double gone = v * ( clock.t[ f ] - T );
					double wx = 0.0, wy = 0.0;
					if( path == Path::Transit )
					{
						const double x0 = Swarm::EntryOf( s.seed, p.slot, 0 ), half = std::sqrt( 1.0 - x0 * x0 );
						wx = x0;
						wy = half - gone;
					}
					else
					{
						wx = ( 1.0 - gone ) * std::sin( p.bearing );
						wy = ( 1.0 - gone ) * std::cos( p.bearing );
					}
					worst = std::max( { worst, std::fabs( p.x - wx ), std::fabs( p.y - wy ) } );
				}
			}
			int arrived = 0;
			for( uint32_t i = 3; i < 12; ++i )
				arrived += arrive[ i ] >= 0.0;
			Check( startOk && arrived == 9 && lateIn == 0 && outOfPart == 0 && inWindow < 1.0 && inWindow >= 8.0 / 9.0 && worst < 1e-9,
			       fmt( "%s %-9s Start With 3 of 12: 3 on at once, 9 waiting; each came in on the first frame after its moment "
			            "wait / v (%d not), each wait in its own ninth of W = %.4f radii (%d not; the last at %.1f%%); from the rim, on "
			            "its track to %.1e (bound 1e-9)",
			            path == Path::Transit ? "Transit" : "Closing", clock.name, lateIn, W, outOfPart, 100.0 * inWindow, worst ) );
		}
	}

	//5. The promise: every latecomer is on the scope before the first of them
	//leaves (with Start With 0, before any sprite leaves), at every Spread and
	//Stability, on both paths, at steady controls.
	{
		int cases = 0, broken = 0;
		double tightest = 1e30;
		std::string what;
		for( Path path : { Path::Closing, Path::Transit } )
			for( double spread : { 0.0, 0.35, 1.0 } )
				for( double stability : { 1.0, 0.75, 0.0 } )
					for( int startWith : { 0, 4 } )
					{
						Settings s;
						s.count     = 32;
						s.seed      = 11;
						s.speed     = 0.1;
						s.spread    = spread;
						s.stability = stability;
						s.path      = path;
						s.startWith = startWith;
						Swarm sw;
						configure( sw );
						sw.Step( 0.0, s );
						double lastIn = -1.0, firstOut = -1.0;
						std::vector< uint32_t > generation( 32, 0 );
						for( int f = 1; f < 60 * 120 && firstOut < 0.0; ++f )
						{
							sw.Step( 1.0 / 60.0, s );
							const double t = f / 60.0;
							if( sw.Waiting() > 0 )
								lastIn = t;
							for( const swarm::Placed& p : sw.Sprites() )
								if( static_cast< int >( p.slot ) >= startWith && p.generation > 0 && firstOut < 0.0 )
									firstOut = t;
						}
						++cases;
						const bool ok = firstOut > 0.0 && lastIn < firstOut;
						broken += !ok;
						if( ok )
							tightest = std::min( tightest, firstOut - lastIn );
						else
							what += fmt( " %s/%.2f/%.2f/%d: last in %.2f s, first out %.2f s;", path == Path::Transit ? "Transit" : "Closing",
							             spread, stability, startWith, lastIn, firstOut );
					}
		Check( broken == 0,
		       fmt( "%d cases (both paths, Spread 0/0.35/1, Stability 1/0.75/0, Start With 0 and 4, 32 sprites at 0.1 radii/s): in %d of "
		            "them a latecomer left before the last came in; the closest call %.2f s%s",
		            cases, broken, tightest, what.c_str() ) );
	}

	//6. Count rising: only enough go on at once to make Start With; the rest
	//wait. Sprite Speed 0 holds the latecomers back with the rest. And it is
	//deterministic: two swarms agree to the bit, another seed does not.
	{
		Settings s;
		s.seed      = 11;
		s.speed     = 0.2;
		s.startWith = 4;
		Swarm sw;
		configure( sw );
		s.count = 6;
		sw.Step( 0.0, s );
		const bool first = sw.Sprites().size() == 4 && sw.Waiting() == 2;
		for( int f = 0; f < 600; ++f )
			sw.Step( 1.0 / 60.0, s );
		const bool allIn = sw.Waiting() == 0;
		s.count          = 10;
		sw.Step( 0.0, s );
		const bool rise = sw.Sprites().size() == 6 && sw.Waiting() == 4;
		s.count         = 3;
		sw.Step( 0.0, s );
		const bool fall = sw.Size() == 3;

		Swarm held;
		configure( held );
		Settings still = s;
		still.count    = 8;
		still.speed    = 0.0;
		still.startWith = 2;
		held.Step( 0.0, still );
		for( int f = 0; f < 600; ++f )
			held.Step( 1.0 / 60.0, still );
		const bool heldBack = held.Waiting() == 6;
		still.speed         = 0.2;
		for( int f = 0; f < 600; ++f )
			held.Step( 1.0 / 60.0, still );
		const bool released = held.Waiting() == 0;

		Settings d;
		d.count     = 16;
		d.seed      = 11;
		d.speed     = 0.1;
		d.spread    = 0.6;
		d.stability = 0.3;
		d.startWith = 2;
		d.path      = Path::Transit;
		const std::vector< double > t = frameTimes( 0.0, 10.0, true );
		Swarm a, b, c;
		configure( a );
		configure( b );
		configure( c );
		Settings other = d;
		other.seed     = 12;
		int differ = 0, order = 0;
		for( size_t i = 0; i < t.size(); ++i )
		{
			const double dt = i == 0 ? 0.0 : t[ i ] - t[ i - 1 ];
			a.Step( dt, d );
			b.Step( dt, d );
			c.Step( dt, other );
			const auto pa = a.Sprites(), pb = b.Sprites();
			differ += pa.size() != pb.size() || a.Waiting() != b.Waiting();
			for( size_t k = 0; k < std::min( pa.size(), pb.size() ); ++k )
				differ += pa[ k ].x != pb[ k ].x || pa[ k ].y != pb[ k ].y || pa[ k ].slot != pb[ k ].slot;
			order += a.Waiting() != c.Waiting();
		}
		Check( first && allIn && rise && fall && heldBack && released && differ == 0 && order > 0,
		       fmt( "Start With 4: Count 0 -> 6 puts 4 on and 2 waiting (%s), all in within 3 s (%s); 6 -> 10 puts none on, 4 waiting "
		            "(%s); -> 3 keeps 3 (%s). Sprite Speed 0 holds 6 latecomers back for 10 s (%s), and 0.2 lets them in (%s). Two "
		            "swarms on one jittered clock: %d differences; Seed 12 arrives differently on %d frames",
		            first ? "yes" : "NO", allIn ? "yes" : "NO", rise ? "yes" : "NO", fall ? "yes" : "NO", heldBack ? "yes" : "NO",
		            released ? "yes" : "NO", differ, order ) );
	}
	return Verdict();
}

//===========================================================================
// --sprite-transit: Transit's track, read off the phosphor column by column.
//===========================================================================
/// One painted column of one sprite: when it was painted, its bearing, and
/// the centroid of the sprite's flash along it (the pulse's half taken off).
struct ColumnReading
{
	uint32_t generation;
	double t, theta, r, want;
};

/// After a frame: every column the antenna crossed in it that runs through
/// the disc of `radius` at ( px, py ) far enough from its edge, the centre
/// and the rim, read from the sprites' flash share.
void readColumns( Rig& rig, double b0, double b1, double px, double py, double radius, double pulse, double t, uint32_t generation,
                  std::vector< ColumnReading >& out )
{
	const double bin = 1.0 / kRanges;
	for( int64_t k = sweep::floorToInt( b0 ) + 1; k <= sweep::floorToInt( b1 ); ++k )
	{
		const int col      = sweep::Column( k, kBearings );
		const double theta = bearingOfBin( col );
		const double ux = std::sin( theta ), uy = std::cos( theta );
		const double along = px * ux + py * uy, off = std::fabs( px * uy - py * ux );
		if( along <= 0.0 || off > 0.6 * radius )
			continue;
		const double c = std::sqrt( radius * radius - off * off );
		if( along - c < 3.0 * bin || along + c + pulse > 1.0 - 3.0 * bin )
			continue;
		const Floats column = rig.PhosphorRect4( col, 0, 1, kRanges );
		const int first     = std::max( 0, static_cast< int >( ( along - c ) / bin ) - 6 );
		const int last      = std::min( kRanges - 1, static_cast< int >( ( along + c + pulse ) / bin ) + 6 );
		double sum = 0.0, moment = 0.0;
		for( int i = first; i <= last; ++i )
		{
			const double value = column[ static_cast< size_t >( i ) * 4 + 2 ];
			sum += value;
			moment += value * ( i + 0.5 ) * bin;
		}
		if( sum > 0.0 )
			out.push_back( { generation, t, theta, moment / sum - 0.5 * pulse, along } );
	}
}

/// Least squares of r = x0 sin( theta ) + ( y0 - v tau ) cos( theta ): the
/// straight southward track that best explains the columns.
bool fitTrack( const std::vector< ColumnReading >& rows, double tStart, double& x0, double& y0, double& v )
{
	double A[ 3 ][ 3 ] = {}, b[ 3 ] = {};
	for( const ColumnReading& c : rows )
	{
		const double f[ 3 ] = { std::sin( c.theta ), std::cos( c.theta ), -( c.t - tStart ) * std::cos( c.theta ) };
		for( int i = 0; i < 3; ++i )
		{
			for( int j = 0; j < 3; ++j )
				A[ i ][ j ] += f[ i ] * f[ j ];
			b[ i ] += f[ i ] * c.r;
		}
	}
	//Gauss-Jordan on 3 x 3, partial pivoting.
	for( int col = 0; col < 3; ++col )
	{
		int pivot = col;
		for( int r = col + 1; r < 3; ++r )
			if( std::fabs( A[ r ][ col ] ) > std::fabs( A[ pivot ][ col ] ) )
				pivot = r;
		if( std::fabs( A[ pivot ][ col ] ) < 1e-300 )
			return false;
		std::swap( A[ pivot ], A[ col ] );
		std::swap( b[ pivot ], b[ col ] );
		for( int r = 0; r < 3; ++r )
			if( r != col )
			{
				const double m = A[ r ][ col ] / A[ col ][ col ];
				for( int j = 0; j < 3; ++j )
					A[ r ][ j ] -= m * A[ col ][ j ];
				b[ r ] -= m * b[ col ];
			}
	}
	x0 = b[ 0 ] / A[ 0 ][ 0 ];
	y0 = b[ 1 ] / A[ 1 ][ 1 ];
	v  = b[ 2 ] / A[ 2 ][ 2 ];
	return true;
}

int runSpriteTransit( const Perturb& perturb )
{
	std::printf( "\n=== sprite-transit: on Transit a sprite runs straight down its chord at Sprite Speed, leaves at the bottom rim and "
	             "the next comes in at the top -- read off the phosphor column by column; Stability weaves it sideways; Grow On "
	             "Approach is the range law\n" );
	const double v      = SpriteSpeedFromParam( ParamFromSpriteSpeed( 0.25 ) );
	const double pulse  = kMetresPerMicrosecond * PulseFromParam( ParamFromPulse( 0.05 ) ) / 1000.0 / 24.0;
	const double size   = SpriteSizeFromParam( ParamFromSpriteSize( 0.05 ) );
	const double radius = 0.25 * size;//the blip at Threshold 0.5: a disc Size / 2 across
	const uint32_t seed = 11;
	const double bin    = 1.0 / kRanges;

	//1. Steady. The law, stated here: generation 0 is StartOf( 0 ) of its chord
	//in, then falls at v; at the bottom rim (the first frame at or after) the
	//next comes in at the top rim of its own chord and falls at v.
	const double x0a = swarm::Swarm::EntryOf( seed, 0, 0 ), x0b = swarm::Swarm::EntryOf( seed, 0, 1 );
	const double ha = std::sqrt( 1.0 - x0a * x0a ), hb = std::sqrt( 1.0 - x0b * x0b );
	const double ya     = ha - swarm::Swarm::StartOf( 0 ) * 2.0 * ha;
	const double tStar  = ( ya + ha ) / v;
	for( const Raster& raster : kRasters )
		for( double fps : { 60.0, 24.0 } )
		{
			Rig rig;
			rig.fps = fps;
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			spriteScene( rig );
			rig.plugin.SetSpriteTransitNorthForTest( perturb.transitNorth );
			rig.Set( PT_RPM, ParamFromRpm( 120.0 ) );
			rig.Set( PT_PERSISTENCE, ParamFromPersistence( 0.1 ) );
			rig.Set( PT_SPRITE_COUNT, 1.0f );
			rig.Set( PT_SPRITE_SIZE, ParamFromSpriteSize( 0.05 ) );
			rig.Set( PT_SPRITE_SPEED, ParamFromSpriteSpeed( 0.25 ) );
			rig.Set( PT_SPRITE_PATH, static_cast< float >( SpritePath::Transit ) );
			double tRespawn = -1.0, seenRespawn = -1.0;
			const int frames = static_cast< int >( ( tStar + 0.8 * 2.0 * hb / v ) * fps );
			std::vector< ColumnReading > rows;
			for( int f = 0; f < frames; ++f )
			{
				const double b0 = rig.plugin.AntennaBins();
				if( !rig.Render( 1 ) )
					return 1;
				const double t = f / fps;
				if( tRespawn < 0.0 && t >= tStar - 1e-9 )
					tRespawn = t;
				if( seenRespawn < 0.0 && !rig.plugin.SpritesForTest().empty() && rig.plugin.SpritesForTest()[ 0 ].generation == 1 )
					seenRespawn = t;
				const bool second = tRespawn >= 0.0;
				const double px   = second ? x0b : x0a;
				const double py   = second ? hb - v * ( t - tRespawn ) : ya - v * t;
				readColumns( rig, b0, rig.plugin.AntennaBins(), px, py, radius, pulse, t, second ? 1u : 0u, rows );
			}
			double worst = 0.0;
			int n[ 2 ]   = { 0, 0 };
			std::vector< ColumnReading > part[ 2 ];
			for( const ColumnReading& c : rows )
			{
				worst = std::max( worst, std::fabs( c.r - c.want ) );
				part[ c.generation ].push_back( c );
				++n[ c.generation ];
			}
			double fx[ 2 ] = {}, fy[ 2 ] = {}, fv[ 2 ] = {};
			const bool fitted = n[ 0 ] >= 20 && n[ 1 ] >= 20 && fitTrack( part[ 0 ], 0.0, fx[ 0 ], fy[ 0 ], fv[ 0 ] )
			                    && fitTrack( part[ 1 ], tRespawn, fx[ 1 ], fy[ 1 ], fv[ 1 ] );
			//Half a range bin a column: each end of a chord is placed to half a
			//bin by sampling at bin centres (the 2-tap pulse average splits that
			//step, it does not remove it), so its midpoint is good to half a bin.
			//The beam's average over neighbouring columns only smooths it.
			Check( fitted && worst <= 0.5 * bin,
			       fmt( "%dx%d %2.0f fps: %d + %d painted columns of the sprite, each where the straight southward track at %.3f radii/s "
			            "puts it, to %.3f bins (bound 0.5)",
			            raster.w, raster.h, fps, n[ 0 ], n[ 1 ], v, worst / bin ) );
			//Fitted freely, the columns give back the track: x held at the entry
			//point, y falling at Sprite Speed; the second track starts at the top rim.
			const bool track = fitted && std::fabs( fv[ 0 ] - v ) <= 1e-3 * v && std::fabs( fv[ 1 ] - v ) <= 1e-3 * v
			                   && std::fabs( fx[ 0 ] - x0a ) <= 0.5 * bin && std::fabs( fx[ 1 ] - x0b ) <= 0.5 * bin
			                   && std::fabs( fy[ 1 ] - hb ) <= 0.5 * bin;
			const bool exit = std::fabs( seenRespawn - tRespawn ) < 0.5 / fps;
			Check( track && exit,
			       fmt( "%dx%d %2.0f fps: fitted, track 1 runs south at %.5f radii/s along x = %.5f (%.5f), track 2 at %.5f along x = "
			            "%.5f (%.5f) from y = %.5f (the top rim, %.5f); the first left at the bottom rim at %.3f s (law %.3f)",
			            raster.w, raster.h, fps, fv[ 0 ], fx[ 0 ], x0a, fv[ 1 ], fx[ 1 ], x0b, fy[ 1 ], hb, seenRespawn, tRespawn ) );
		}

	//2. Stability 0: the painted columns follow the sprite where the swarm
	//put it each frame, and those places weave across the track while the
	//sprite keeps going south and stays inside the scope.
	for( const Raster& raster : kRasters )
	{
		Rig rig;
		if( !rig.Init( raster.w, raster.h ) )
			return 1;
		spriteScene( rig );
		rig.plugin.SetSpriteTransitNorthForTest( perturb.transitNorth );
		rig.Set( PT_RPM, ParamFromRpm( 120.0 ) );
		rig.Set( PT_PERSISTENCE, ParamFromPersistence( 0.1 ) );
		rig.Set( PT_SPRITE_COUNT, 1.0f );
		rig.Set( PT_SPRITE_SIZE, ParamFromSpriteSize( 0.05 ) );
		rig.Set( PT_SPRITE_SPEED, ParamFromSpriteSpeed( 0.25 ) );
		rig.Set( PT_STABILITY, 0.0f );
		rig.Set( PT_SPRITE_PATH, static_cast< float >( SpritePath::Transit ) );
		std::vector< ColumnReading > rows;
		double swing = 0.0, lastY = 2.0, farthest = 0.0;
		int north = 0;
		for( int f = 0; f < 360; ++f )
		{
			const double b0 = rig.plugin.AntennaBins();
			if( !rig.Render( 1 ) )
				return 1;
			const auto s = rig.plugin.SpritesForTest()[ 0 ];
			if( s.generation != 0 )
				break;
			readColumns( rig, b0, rig.plugin.AntennaBins(), s.x, s.y, radius, pulse, f / 60.0, 0, rows );
			swing = std::max( swing, std::fabs( s.x - x0a ) );
			north += f > 0 && s.y >= lastY;
			lastY    = s.y;
			farthest = std::max( farthest, s.range );
		}
		double worst = 0.0, straight = 0.0;
		for( const ColumnReading& c : rows )
			worst = std::max( worst, std::fabs( c.r - c.want ) );
		//The best straight southward track through the same columns misses
		//them by far more: the weave is in the picture.
		double fx = 0.0, fy = 0.0, fv = 0.0;
		if( fitTrack( rows, 0.0, fx, fy, fv ) )
			for( const ColumnReading& c : rows )
				straight = std::max( straight, std::fabs( c.r - ( fx * std::sin( c.theta ) + ( fy - fv * c.t ) * std::cos( c.theta ) ) ) );
		Check( rows.size() >= 20 && worst <= 0.5 * bin && swing > 0.03 && north == 0 && farthest <= 1.0,
		       fmt( "%dx%d  Stability 0: %zu painted columns where the swarm put the sprite to %.3f bins (bound 0.5); it swung %.4f "
		            "radii across the track (no straight track fits the paint closer than %.1f bins), went north %d times and stayed "
		            "inside the scope",
		            raster.w, raster.h, rows.size(), worst / bin, swing, straight / bin, north ) );
	}

	//3. Grow On Approach on Transit: the range law, Size x ( 1 + 3 g ( 1 - R ) ),
	//so a sprite is biggest at its closest approach. Held still, squares, Face
	//Centre so the height runs along the ray, g = 1.
	for( const Raster& raster : kRasters )
	{
		const std::string square = scratchFolder() + "/square.png";
		writeAlphaSprite( square, 16, 16, []( int, int ) { return 1.0; } );
		Rig rig;
		if( !rig.Init( raster.w, raster.h ) )
			return 1;
		spriteScene( rig );
		rig.plugin.SetDecayForTest( false );
		rig.plugin.SetSpriteTransitGrowByTravelForTest( perturb.transitGrowByTravel );
		rig.Set( PT_RPM, ParamFromRpm( 120.0 ) );
		rig.Set( PT_SPRITE_COUNT, 6.0f );
		rig.Set( PT_SPRITE_SIZE, ParamFromSpriteSize( 0.04 ) );
		rig.Set( PT_ORIENTATION, static_cast< float >( Orientation::FaceCentre ) );
		rig.Set( PT_SPRITE_GROW, 1.0f );
		rig.Set( PT_SPRITE_PATH, static_cast< float >( SpritePath::Transit ) );
		setSpriteFile( rig, square );
		if( !rig.Render( 31 ) )
			return 1;
		const double side = SpriteSizeFromParam( ParamFromSpriteSize( 0.04 ) );
		int measured = 0, wrong = 0;
		std::string what;
		const auto sprites = rig.plugin.SpritesForTest();
		for( size_t i = 0; i < sprites.size(); ++i )
		{
			const auto& s      = sprites[ i ];
			const double grown = side * ( 1.0 + 3.0 * ( 1.0 - s.range ) );
			if( !isolated( sprites, i ) )
				continue;
			const double polar = radialExtent( rig, s, 0.75 * grown );
			const double x0    = swarm::Swarm::EntryOf( seed, s.slot, s.generation );
			++measured;
			wrong += std::fabs( polar - grown ) > 1.5 / kRanges;
			what += fmt( " R %.2f (%.0f%% along, closest %.2f): %.4f (%.4f; at closest %.4f);", s.range, 100.0 * s.progress, std::fabs( x0 ),
			             polar, grown, side * ( 1.0 + 3.0 * ( 1.0 - std::fabs( x0 ) ) ) );
		}
		Check( measured >= 3 && wrong == 0,
		       fmt( "%dx%d  Transit, Grow On Approach 1, Size %.3f: across, against Size x (1 + 3 (1 - R)) to 1.5 bins:%s", raster.w,
		            raster.h, side, what.c_str() ) );
	}
	return Verdict();
}

//===========================================================================
// --sprite-arrivals: Start With, read off the phosphor rotation by rotation.
//===========================================================================
/// What one rotation painted: the phosphor now less the phosphor a rotation
/// ago (decay off, so a paint only adds), counted as blobs of the sprites'
/// afterglow share.
int rotationBlobs( const Floats& now, const Floats& before )
{
	Floats diff( now.size() );
	for( size_t i = 0; i < now.size(); ++i )
		diff[ i ] = now[ i ] - before[ i ];
	return countPolarBlobs( diff, 3, 0.05 );
}

/// The sprites on the scope, grouped: those whose discs (with `pad` round
/// them) touch are one blob.
int clusters( const std::vector< RadarPlugin::SpriteDraw >& s, double reach )
{
	std::vector< int > group( s.size() );
	for( size_t i = 0; i < s.size(); ++i )
		group[ i ] = static_cast< int >( i );
	std::function< int( int ) > root = [ & ]( int i ) { return group[ i ] == i ? i : group[ i ] = root( group[ i ] ); };
	for( size_t i = 0; i < s.size(); ++i )
		for( size_t j = i + 1; j < s.size(); ++j )
			if( std::hypot( s[ i ].x - s[ j ].x, s[ i ].y - s[ j ].y ) < reach )
				group[ root( static_cast< int >( i ) ) ] = root( static_cast< int >( j ) );
	std::set< int > roots;
	for( size_t i = 0; i < s.size(); ++i )
		roots.insert( root( static_cast< int >( i ) ) );
	return static_cast< int >( roots.size() );
}

int runSpriteArrivals( const Perturb& perturb )
{
	std::printf( "\n=== sprite-arrivals: Start With below Sprite Count puts that many on at once and brings the rest in at the rim at "
	             "their hashed moments; the painted count follows; all are in before the first latecomer leaves; the same twice\n" );
	const double bin = 1.0 / kRanges;

	//1. The schedule, steady (Spread 0, Stability 1), so every moment is the
	//harness's own: Start With 2 of 8 at 0.08 radii/s. Latecomer i comes in
	//at T_i = wait_i / v, wait_i < W = 0.85 radii; a sprite placed at StartOf( i )
	//leaves at ( 1 - StartOf( i ) ) / v, a latecomer at T_i + 1 / v. A
	//rotation with no arrival and no departure in it paints exactly the
	//sprites on, grouped where their discs touch.
	for( const Raster& raster : kRasters )
	{
		Rig rig;
		if( !rig.Init( raster.w, raster.h ) )
			return 1;
		spriteScene( rig, 0.0 );
		rig.plugin.SetDecayForTest( false );
		rig.plugin.SetSpriteArrivalNaiveForTest( perturb.arrivalNaive );
		rig.Set( PT_RPM, ParamFromRpm( 120.0 ) );//a rotation is 30 frames
		rig.Set( PT_SPRITE_COUNT, 8.0f );
		rig.Set( PT_SPRITE_START, 2.0f );
		rig.Set( PT_SPRITE_SIZE, ParamFromSpriteSize( 0.04 ) );
		rig.Set( PT_SPRITE_SPEED, ParamFromSpriteSpeed( 0.08 ) );
		const double v = SpriteSpeedFromParam( ParamFromSpriteSpeed( 0.08 ) );
		swarm::Settings s;
		s.count = 8;
		s.seed  = 11;
		s.speed = v;
		s.startWith = 2;
		const double W = arrivalWindowLaw( swarm::Path::Closing, 0.0, 1.0 );
		std::vector< double > events;//arrivals and departures, seconds
		int outOfPart = 0;//the j-th of the 6 latecomers must wait inside the j-th sixth of W
		for( uint32_t i = 2; i < 8; ++i )
		{
			const double wait = swarm::Swarm::WaitOf( s, i, static_cast< int >( i ) - 2, 6 );
			outOfPart += !( wait >= ( i - 2 ) * W / 6.0 && wait < ( i - 1 ) * W / 6.0 );
			const double T = wait / v;
			events.push_back( T );
			events.push_back( T + 1.0 / v );
		}
		for( uint32_t i = 0; i < 2; ++i )
			events.push_back( ( 1.0 - swarm::Swarm::StartOf( i ) ) / v );
		const double reach = 2.0 * 0.25 * SpriteSizeFromParam( ParamFromSpriteSize( 0.04 ) ) + 8.0 * bin;
		const int rotations = static_cast< int >( std::ceil( 1.15 * W / v / 0.5 ) );
		if( !rig.Render( 1 ) )
			return 1;
		Floats before = rig.Phosphor4();
		int compared = 0, wrong = 0, first = -1, last = -1;//the law's count at the first and last quiet rotation
		std::string what;
		for( int n = 0; n < rotations; ++n )
		{
			if( !rig.Render( 30 ) )
				return 1;
			const Floats now = rig.Phosphor4();
			const double t0 = n * 0.5, t1 = ( n + 1 ) * 0.5;
			bool quiet = true;
			for( double e : events )
				quiet = quiet && !( e > t0 - 1.0 / 60.0 && e <= t1 + 1.0 / 60.0 );
			int arrived = 2;
			for( uint32_t i = 2; i < 8; ++i )
				arrived += swarm::Swarm::WaitOf( s, i, static_cast< int >( i ) - 2, 6 ) / v <= t0;
			const int painted = rotationBlobs( now, before );
			before            = now;
			if( !quiet )
				continue;
			//The law's count, grouped by where the sprites are (the swarm's
			//positions say only which touch).
			const int expect = arrived - ( static_cast< int >( rig.plugin.SpritesForTest().size() ) - clusters( rig.plugin.SpritesForTest(), reach ) );
			++compared;
			wrong += painted != expect;
			if( first < 0 )
				first = arrived;
			last = arrived;
			what += fmt( " %.1fs:%d/%d", t1, painted, expect );
		}
		Check( compared >= 8 && wrong == 0 && first <= 3 && last == 8 && outOfPart == 0,
		       fmt( "%dx%d  Start With 2 of 8, W = %.3f radii (%.2f s), each latecomer in its own sixth of it (%d not): %d quiet rotations, "
		            "the painted count the schedule's in all but %d (painted/law:%s)",
		            raster.w, raster.h, W, W / v, outOfPart, compared, wrong, what.c_str() ) );
	}

	//2. The promise, at the sprites' default Spread and Stability, on
	//Transit, Start With 0: the painted count reaches Count before any sprite
	//has left. A latecomer is painted within a rotation of coming in, so the
	//reading lags by up to two rotations (a second): at 0.06 radii/s the law
	//leaves 1.5 s between the last arrival and the quickest possible exit.
	//The first exit comes much later than the picture fills, so it is found
	//on a replica of the plugin's swarm, stepped on the same clock -- one that
	//agrees with the plugin's own to the bit at the last frame rendered.
	//And the same settings twice paint the same phosphor.
	for( const Raster& raster : kRasters )
	{
		Rig a, b;
		if( !a.Init( raster.w, raster.h ) || !b.Init( raster.w, raster.h ) )
			return 1;
		for( Rig* rig : { &a, &b } )
		{
			spriteScene( *rig, 0.0 );
			rig->plugin.SetDecayForTest( false );
			rig->plugin.SetSpriteArrivalNaiveForTest( perturb.arrivalNaive );
			rig->plugin.SetSpriteWallSeedForTest( perturb.spriteWallSeed );
			rig->Set( PT_RPM, ParamFromRpm( 120.0 ) );
			rig->Set( PT_SPRITE_COUNT, 10.0f );
			rig->Set( PT_SPRITE_START, 0.0f );
			rig->Set( PT_SPRITE_SIZE, ParamFromSpriteSize( 0.03 ) );
			rig->Set( PT_SPRITE_SPEED, ParamFromSpriteSpeed( 0.06 ) );
			rig->Set( PT_SPEED_SPREAD, 0.35f );
			rig->Set( PT_STABILITY, 0.75f );
			rig->Set( PT_SPRITE_PATH, static_cast< float >( SpritePath::Transit ) );
		}
		const double reach = 2.0 * 0.25 * SpriteSizeFromParam( ParamFromSpriteSize( 0.03 ) ) + 8.0 * bin;
		if( !a.Render( 1 ) || !b.Render( 1 ) )
			return 1;
		Floats before = a.Phosphor4();
		double full = -1.0;
		int differ = 0, waitDiffer = 0, frames = 1;
		for( int n = 0; n < 40 && ( full < 0.0 || n < 12 ); ++n )
		{
			for( int f = 0; f < 30; ++f )
			{
				if( !a.Render( 1 ) || ( n < 12 && !b.Render( 1 ) ) )
					return 1;
				if( n < 12 )
					waitDiffer += a.plugin.SpritesWaitingForTest() != b.plugin.SpritesWaitingForTest();
				++frames;
			}
			const Floats now = a.Phosphor4();
			if( n < 12 && b.Phosphor4() != now )
				++differ;
			const int painted = rotationBlobs( now, before );
			before            = now;
			const auto on     = a.plugin.SpritesForTest();
			if( full < 0.0 && a.plugin.SpritesWaitingForTest() == 0 && painted == clusters( on, reach ) && on.size() == 10 )
				full = ( n + 1 ) * 0.5;
		}
		//The replica: the settings the plugin derives from these controls, on
		//the rig's clock (frame f at f / 60 s, so the same frame lengths).
		swarm::Settings s;
		s.count     = 10;
		s.seed      = 11;
		s.speed     = SpriteSpeedFromParam( ParamFromSpriteSpeed( 0.06 ) );
		s.spread    = SpeedSpreadFromParam( 0.35f );
		s.stability = StabilityFromParam( 0.75f );
		s.path      = swarm::Path::Transit;
		s.startWith = 0;
		swarm::Swarm replica;
		replica.SetArrivalNaiveForTest( perturb.arrivalNaive );
		double firstOut = -1.0;
		bool same       = false;
		for( int f = 0; f < 60 * 120 && firstOut < 0.0; ++f )
		{
			replica.Step( f == 0 ? 0.0 : f / 60.0 - ( f - 1 ) / 60.0, s );
			if( f == frames - 1 )
			{
				const auto mine = replica.Sprites();
				const auto its  = a.plugin.SpritesForTest();
				same            = mine.size() == its.size() && replica.Waiting() == a.plugin.SpritesWaitingForTest();
				for( size_t k = 0; same && k < mine.size(); ++k )
					same = mine[ k ].x == its[ k ].x && mine[ k ].y == its[ k ].y;
			}
			for( const swarm::Placed& p : replica.Sprites() )
				if( p.generation > 0 && firstOut < 0.0 )
					firstOut = f / 60.0;
		}
		Check( full > 0.0 && firstOut > 0.0 && full < firstOut && same,
		       fmt( "%dx%d  Transit, Start With 0 of 10, Spread 0.35, Stability 0.75: a rotation painted all 10 by %.1f s, and the "
		            "first left at %.2f s (the swarm stepped on, which matched the plugin's to the bit at %.2f s: %s)",
		            raster.w, raster.h, full, firstOut, ( frames - 1 ) / 60.0, same ? "yes" : "NO" ) );
		Check( differ == 0 && waitDiffer == 0,
		       fmt( "%dx%d  two instances, the same settings and clock: %d of 12 rotations' phosphor differ, %d frames' latecomers differ",
		            raster.w, raster.h, differ, waitDiffer ) );
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
		{ "sprite-utf16", runSpriteUtf16, true }, { "sprite-motion", runSpriteMotion, true },
		{ "sprite-file", runSpriteFile, false },  { "sprite-threshold", runSpriteThreshold, false },
		{ "sprites", runSprites, false },         { "sprite-speed", runSpriteSpeed, false },
		{ "sprite-beam", runSpriteBeam, false },  { "sprite-repeat", runSpriteRepeat, false },
		{ "sprite-colour", runSpriteColour, false },
		{ "sprite-path", runSpritePath, true },   { "sprite-transit", runSpriteTransit, false },
		{ "sprite-arrivals", runSpriteArrivals, false },
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
	add( "sprite-utf16", runSpriteUtf16, "widen a path byte by byte, as a narrow fopen reads it",
	     []( Perturb& p ) { p.spriteUtf16Bytes = true; } );
	add( "sprite-motion", runSpriteMotion, "add Sprite Speed / 60 a frame, whatever the frame's length",
	     []( Perturb& p ) { p.spriteMotionPerFrame = true; } );
	add( "sprite-file", runSpriteFile, "leave the last good picture up when a file fails",
	     []( Perturb& p ) { p.spriteKeepLast = true; } );
	add( "sprite-threshold", runSpriteThreshold, "take any alpha at all as the target",
	     []( Perturb& p ) { p.spriteThresholdIgnored = true; } );
	add( "sprites", runSprites, "draw Face Centre upright", []( Perturb& p ) { p.spriteOrientationIgnored = true; } );
	add( "sprite-speed", runSpriteSpeed, "tie the plugin's sprite motion to frames, not time",
	     []( Perturb& p ) { p.spriteSpeedPerFrame = true; } );
	add( "sprite-beam", runSpriteBeam, "never fade the sprites' share", []( Perturb& p ) { p.spriteNoFade = true; } );
	add( "sprite-repeat", runSpriteRepeat, "salt the sprites' seed from the wall clock", []( Perturb& p ) { p.spriteWallSeed = true; } );
	add( "sprite-colour", runSpriteColour, "turn Hue the other way", []( Perturb& p ) { p.spriteHueBackwards = true; } );
	//Since v0.2.0.
	add( "sprites", runSprites, "draw Head In as Face Centre", []( Perturb& p ) { p.headInAsFace = true; } );
	add( "sprite-colour", runSpriteColour, "read the picture's colour as BGR", []( Perturb& p ) { p.colourSwapped = true; } );
	add( "sprite-path", runSpritePath, "spread Transit's entry points across the whole width",
	     []( Perturb& p ) { p.transitEntryWide = true; } );
	add( "sprite-path", runSpritePath, "spread the latecomers over one crossing at Sprite Speed, ignoring Spread and the surge",
	     []( Perturb& p ) { p.arrivalNaive = true; } );
	add( "sprite-transit", runSpriteTransit, "run Transit's tracks north", []( Perturb& p ) { p.transitNorth = true; } );
	add( "sprite-transit", runSpriteTransit, "grow Transit's sprites by the share of the track done, not by range",
	     []( Perturb& p ) { p.transitGrowByTravel = true; } );
	add( "sprite-arrivals", runSpriteArrivals, "spread the latecomers over one crossing at Sprite Speed, ignoring Spread and the surge",
	     []( Perturb& p ) { p.arrivalNaive = true; } );
	add( "sprite-arrivals", runSpriteArrivals, "salt the sprites' seed from the wall clock",
	     []( Perturb& p ) { p.spriteWallSeed = true; } );

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
			             "          --sprite-file --sprite-threshold --sprites --sprite-speed --sprite-beam\n"
			             "          --sprite-repeat --sprite-colour --sprite-transit --sprite-arrivals\n"
			             "          --sweep-law --cues --names --sprite-utf16 --sprite-motion --sprite-path (no GL)\n"
			             "          --negative   --bench\n"
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
				else if( p.type == FF_TYPE_FILE )
					line += "\"type\": \"ParamFile\", \"default\": \"\"";
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
				//The sprites are off by default (radar#5): every other sprite
				//control shows only with some on, and Orientation only with a
				//picture that is not round.
				if( std::string( GroupOf( plugin.IdOfHostForTest( p.index ) ) ) == "Sprites" && p.name != "Sprite Count" )
					line += ", \"needs\": {\"Sprite Count\": 10.0}";
				if( p.name == "Orientation" )
					line += ", \"note\": \"needs an asymmetric Sprite File to show (tools/fixtures/alien.png)\"";
				//Since v0.2.0. Start With is an integer like Sprite Count; it and
				//Path act only on sprites that START after the change.
				if( p.name == "Start With" )
					line += ", \"probe\": [0, 4], \"note\": \"acts only when sprites start (a new instance, or Sprite Count rising): on a "
					        "running layer a probe changes nothing until Sprite Count is raised from below it (the gate's blind spot)\"";
				if( p.name == "Path" )
					line += ", \"note\": \"latched per sprite: one in flight finishes on its old path, so a change shows as tracks end "
					        "(25 s a crossing at the default Sprite Speed); set it before raising Sprite Count, or raise Sprite Speed\"";
				if( p.name == "Sprite Colour" )
					line += ", \"note\": \"needs a coloured Sprite File to show (tools/fixtures/alien.png is green); the blip and grey "
					        "pictures fall back to Phosphor\"";
				if( p.name == "Sprite File" )
					line += ", \"note\": \"a fixture to load: tools/fixtures/alien.png (empty is the built-in blip)\"";
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
