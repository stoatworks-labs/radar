#include "SpriteImage.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <system_error>

// stb_image is compiled here and only here (shunt's arrangement). Decoding is
// from memory, so stb's own fopen is never used: STBI_NO_STDIO keeps the narrow
// path out entirely. STBI_NO_HDR and STBI_NO_PIC drop the float paths an
// 8-bit mask could not use; failure strings are kept, because
// stbi_failure_reason() is what tells "not an image" from "truncated" in the log.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_PIC
#if defined( __clang__ )
	#pragma clang diagnostic push
	#pragma clang diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined( __clang__ )
	#pragma clang diagnostic pop
#endif

namespace radar::sprite
{
namespace
{
/// One axis of the box filter: every row of a w x h grid resampled from w to
/// newW texels, each output the area-weighted mean of the source texels it
/// covers. The vertical pass is the same thing on the transpose.
std::vector< float > reduceRows( const std::vector< float >& in, int w, int h, int newW )
{
	std::vector< float > out( static_cast< size_t >( newW ) * h, 0.0f );
	const double scale = static_cast< double >( w ) / newW;//source pixels per output pixel
	for( int x = 0; x < newW; ++x )
	{
		const double x0 = x * scale, x1 = ( x + 1 ) * scale;
		const int first = static_cast< int >( std::floor( x0 ) );
		const int last  = std::min( w - 1, static_cast< int >( std::ceil( x1 ) ) - 1 );
		for( int y = 0; y < h; ++y )
		{
			double sum = 0.0;
			for( int s = first; s <= last; ++s )
			{
				const double cover = std::min( x1, s + 1.0 ) - std::max( x0, static_cast< double >( s ) );
				if( cover > 0.0 )
					sum += cover * in[ static_cast< size_t >( y ) * w + s ];
			}
			out[ static_cast< size_t >( y ) * newW + x ] = static_cast< float >( sum / scale );
		}
	}
	return out;
}

std::vector< float > transpose( const std::vector< float >& in, int w, int h )
{
	std::vector< float > out( in.size() );
	for( int y = 0; y < h; ++y )
		for( int x = 0; x < w; ++x )
			out[ static_cast< size_t >( x ) * h + y ] = in[ static_cast< size_t >( y ) * w + x ];
	return out;
}
} // namespace

const Mask& BuiltIn()
{
	static const Mask blip = [] {
		constexpr int n = 64;
		Mask m;
		m.width = m.height = n;
		m.fromAlpha        = true;
		m.value.resize( static_cast< size_t >( n ) * n );
		for( int y = 0; y < n; ++y )
			for( int x = 0; x < n; ++x )
			{
				//The texel's centre, in units of the box: the cone reaches 0 at
				//the inscribed circle, half the box from the centre.
				const double u = ( x + 0.5 ) / n - 0.5, v = ( y + 0.5 ) / n - 0.5;
				const double d = std::sqrt( u * u + v * v ) / 0.5;
				m.value[ static_cast< size_t >( y ) * n + x ] = static_cast< float >( std::max( 0.0, 1.0 - d ) );
			}
		return m;
	}();
	return blip;
}

Mask Reduce( const std::vector< float >& value, int width, int height, int maxSide )
{
	Mask m;
	const int longer = std::max( width, height );
	if( longer <= maxSide )
	{
		m.width  = width;
		m.height = height;
		m.value  = value;
		return m;
	}
	const double scale = static_cast< double >( maxSide ) / longer;
	const int newW     = std::max( 1, static_cast< int >( std::lround( width * scale ) ) );
	const int newH     = std::max( 1, static_cast< int >( std::lround( height * scale ) ) );
	std::vector< float > across = reduceRows( value, width, height, newW );
	std::vector< float > down   = reduceRows( transpose( across, newW, height ), height, newW, newH );
	m.width                     = newW;
	m.height                    = newH;
	m.value                     = transpose( down, newH, newW );
	return m;
}

Loaded Decode( const unsigned char* bytes, size_t size )
{
	Loaded result;
	if( bytes == nullptr || size == 0 )
	{
		result.why = "is empty";
		return result;
	}
	if( size > static_cast< size_t >( 0x7fffffff ) )
	{
		result.why = "is too large to decode";
		return result;
	}
	int w = 0, h = 0, n = 0;
	if( !stbi_info_from_memory( bytes, static_cast< int >( size ), &w, &h, &n ) )
	{
		const char* reason = stbi_failure_reason();
		result.why         = std::string( "will not decode (" ) + ( reason ? reason : "not an image stb_image reads" ) + ")";
		return result;
	}
	if( w <= 0 || h <= 0 || w > kMaxImageSide || h > kMaxImageSide )
	{
		result.why = std::to_string( w ) + "x" + std::to_string( h ) + " is outside 1.." + std::to_string( kMaxImageSide ) + " px";
		return result;
	}
	stbi_uc* pixels = stbi_load_from_memory( bytes, static_cast< int >( size ), &w, &h, &n, 4 );
	if( pixels == nullptr )
	{
		const char* reason = stbi_failure_reason();
		result.why         = std::string( "will not decode (" ) + ( reason ? reason : "unknown" ) + ")";
		return result;
	}
	result.sourceWidth    = w;
	result.sourceHeight   = h;
	result.sourceChannels = n;

	//Alpha if the picture really has transparency; an alpha channel that is
	//255 everywhere is an opaque picture, and gets the brightest channel.
	bool transparent = false;
	if( n == 2 || n == 4 )
		for( size_t i = 0; i < static_cast< size_t >( w ) * h && !transparent; ++i )
			transparent = pixels[ i * 4 + 3 ] < 255;

	std::vector< float > full( static_cast< size_t >( w ) * h );
	for( int y = 0; y < h; ++y )
	{
		//stb gives the top row first; the mask is bottom first.
		const stbi_uc* row = pixels + static_cast< size_t >( y ) * w * 4;
		float* out         = &full[ static_cast< size_t >( h - 1 - y ) * w ];
		for( int x = 0; x < w; ++x )
		{
			const stbi_uc* p = row + static_cast< size_t >( x ) * 4;
			const int v      = transparent ? p[ 3 ] : std::max( { p[ 0 ], p[ 1 ], p[ 2 ] } );
			out[ x ]         = static_cast< float >( v ) / 255.0f;
		}
	}
	stbi_image_free( pixels );

	result.mask           = Reduce( full, w, h, kMaxMaskSide );
	result.mask.fromAlpha = transparent;
	result.ok             = true;
	return result;
}

bool Utf8ToUtf16( const std::string& in, std::u16string& out )
{
	out.clear();
	const size_t n = in.size();
	size_t i       = 0;
	while( i < n )
	{
		const unsigned char c = static_cast< unsigned char >( in[ i ] );
		char32_t cp           = 0;
		int extra             = 0;
		char32_t minimum      = 0;
		if( c < 0x80 )
		{
			cp = c;
		}
		else if( ( c & 0xE0 ) == 0xC0 )
		{
			cp      = c & 0x1F;
			extra   = 1;
			minimum = 0x80;
		}
		else if( ( c & 0xF0 ) == 0xE0 )
		{
			cp      = c & 0x0F;
			extra   = 2;
			minimum = 0x800;
		}
		else if( ( c & 0xF8 ) == 0xF0 )
		{
			cp      = c & 0x07;
			extra   = 3;
			minimum = 0x10000;
		}
		else
			return false;//a continuation byte where a lead was due, or 0xF8+
		if( i + static_cast< size_t >( extra ) >= n && extra > 0 )
			return false;//truncated: the sequence runs past the end
		for( int k = 1; k <= extra; ++k )
		{
			const unsigned char d = static_cast< unsigned char >( in[ i + static_cast< size_t >( k ) ] );
			if( ( d & 0xC0 ) != 0x80 )
				return false;
			cp = ( cp << 6 ) | ( d & 0x3F );
		}
		if( extra > 0 && cp < minimum )
			return false;//overlong
		if( cp > 0x10FFFF || ( cp >= 0xD800 && cp <= 0xDFFF ) )
			return false;
		if( cp >= 0x10000 )
		{
			const char32_t v = cp - 0x10000;
			out.push_back( static_cast< char16_t >( 0xD800 + ( v >> 10 ) ) );
			out.push_back( static_cast< char16_t >( 0xDC00 + ( v & 0x3FF ) ) );
		}
		else
			out.push_back( static_cast< char16_t >( cp ) );
		i += 1 + static_cast< size_t >( extra );
	}
	return true;
}

std::u16string WidenBytesForTest( const std::string& in )
{
	std::u16string out;
	for( char c : in )
		out.push_back( static_cast< char16_t >( static_cast< unsigned char >( c ) ) );
	return out;
}

bool ReadFileBytes( const std::string& utf8Path, std::vector< unsigned char >& out, std::string& why )
{
	out.clear();
	std::FILE* file = nullptr;
#if defined( _WIN32 )
	std::u16string wide16;
	if( Utf8ToUtf16( utf8Path, wide16 ) )
	{
		const std::wstring wide( wide16.begin(), wide16.end() );
	#if defined( _MSC_VER )
		if( _wfopen_s( &file, wide.c_str(), L"rb" ) != 0 )
			file = nullptr;
	#else
		file = _wfopen( wide.c_str(), L"rb" );
	#endif
	}
	else
	{
		//Not UTF-8 at all: a host handing over its ANSI code page would look
		//like this, and the narrow open reads exactly that. Never tried for a
		//path that IS UTF-8, so it cannot hide the wide open's error.
	#if defined( _MSC_VER )
		if( fopen_s( &file, utf8Path.c_str(), "rb" ) != 0 )
			file = nullptr;
	#else
		file = std::fopen( utf8Path.c_str(), "rb" );
	#endif
		if( file == nullptr )
		{
			why = "is not UTF-8, and cannot be opened in the system code page either";
			return false;
		}
	}
#else
	file = std::fopen( utf8Path.c_str(), "rb" );
#endif
	if( file == nullptr )
	{
		why = "cannot be opened (" + std::generic_category().message( errno ) + ")";
		return false;
	}
	//On the heap: this runs on the host's render thread, whose stack is its own.
	std::vector< unsigned char > chunk( 65536 );
	size_t got = 0;
	while( ( got = std::fread( chunk.data(), 1, chunk.size(), file ) ) > 0 )
	{
		out.insert( out.end(), chunk.begin(), chunk.begin() + static_cast< std::ptrdiff_t >( got ) );
		if( out.size() > kMaxFileBytes )
		{
			std::fclose( file );
			out.clear();
			why = "is larger than " + std::to_string( kMaxFileBytes / ( 1024 * 1024 ) ) + " MB";
			return false;
		}
	}
	const bool failed = std::ferror( file ) != 0;
	std::fclose( file );
	if( failed )
	{
		out.clear();
		why = "could not be read to the end";
		return false;
	}
	if( out.empty() )
	{
		why = "is empty";
		return false;
	}
	return true;
}

Loaded LoadFile( const std::string& utf8Path )
{
	std::vector< unsigned char > bytes;
	std::string why;
	if( !ReadFileBytes( utf8Path, bytes, why ) )
	{
		Loaded failed;
		failed.why = why;
		return failed;
	}
	return Decode( bytes.data(), bytes.size() );
}

} // namespace radar::sprite
