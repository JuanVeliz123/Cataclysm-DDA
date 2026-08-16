#include "godot_light_snapshot.h"

#if defined(GODOT)

#include <algorithm>
#include <cmath>
#include <cstring>

#include <godot_cpp/variant/packed_byte_array.hpp>

#include "lightmap.h"

namespace godot_backend
{

namespace
{
LightSnapshot g_light_snapshot;
} // namespace

LightSnapshot &get_light_snapshot()
{
    return g_light_snapshot;
}

uint8_t encode_light_level( const lit_level ll, const float ambient_light )
{
    float base = 0.0f;
    float span = 0.0f;
    switch( ll ) {
        case lit_level::BRIGHT:
            base = 1.0f;
            break;
        case lit_level::LIT:
            base = 0.88f;
            span = 0.12f;
            break;
        case lit_level::BRIGHT_ONLY:
            // Bright but indistinct. Washed out rather than dark, as before.
            base = 0.80f;
            span = 0.08f;
            break;
        case lit_level::LOW:
            base = 0.30f;
            span = 0.20f;
            break;
        case lit_level::MEMORIZED:
            // Carried by the visibility channel instead; live light does not
            // apply to a tile the character is only remembering.
            break;
        case lit_level::DARK:
        case lit_level::BLANK:
        default:
            base = 0.10f;
            span = 0.05f;
            break;
    }
    const float t = std::clamp( ambient_light / LIGHT_AMBIENT_LIT, 0.0f, 1.0f );
    return static_cast<uint8_t>( std::clamp( base + span * t, 0.0f, 1.0f ) * 255.0f + 0.5f );
}

void LightSnapshot::begin( const int w, const int h )
{
    pending_w_ = std::max( 0, w );
    pending_h_ = std::max( 0, h );
    const size_t want = static_cast<size_t>( pending_w_ ) * pending_h_ * channels;
    pending_.assign( want, 0 );
    // Alpha is unused but must not be zero: an Image with a zero alpha channel
    // is legal, but every tool that ever looks at this texture would show it as
    // empty.
    for( size_t i = 3; i < pending_.size(); i += channels ) {
        pending_[i] = 255;
    }
}

void LightSnapshot::set( const int x, const int y, const uint8_t visibility,
                         const uint8_t light )
{
    if( x < 0 || y < 0 || x >= pending_w_ || y >= pending_h_ ) {
        return;
    }
    const size_t i = ( static_cast<size_t>( y ) * pending_w_ + x ) * channels;
    pending_[i] = visibility;
    pending_[i + 1] = light;
}

void LightSnapshot::set_fire( const int x, const int y, const uint8_t fire )
{
    if( x < 0 || y < 0 || x >= pending_w_ || y >= pending_h_ ) {
        return;
    }
    pending_[( static_cast<size_t>( y ) * pending_w_ + x ) * channels + 2] = fire;
}

void LightSnapshot::blur_fire()
{
    if( pending_w_ <= 0 || pending_h_ <= 0 ) {
        return;
    }
    std::vector<uint8_t> src( static_cast<size_t>( pending_w_ ) * pending_h_ );
    bool any = false;
    for( size_t i = 0; i < src.size(); ++i ) {
        src[i] = pending_[i * channels + 2];
        any = any || src[i] != 0;
    }
    if( !any ) {
        return;
    }
    for( int y = 0; y < pending_h_; ++y ) {
        for( int x = 0; x < pending_w_; ++x ) {
            int sum = 0;
            int count = 0;
            for( int dy = -1; dy <= 1; ++dy ) {
                for( int dx = -1; dx <= 1; ++dx ) {
                    const int nx = x + dx;
                    const int ny = y + dy;
                    if( nx < 0 || ny < 0 || nx >= pending_w_ || ny >= pending_h_ ) {
                        continue;
                    }
                    // Weighted toward the centre, or a fire reads as a square.
                    const int w = ( dx == 0 && dy == 0 ) ? 4 : 1;
                    sum += src[static_cast<size_t>( ny ) * pending_w_ + nx] * w;
                    count += w;
                }
            }
            pending_[( static_cast<size_t>( y ) * pending_w_ + x ) * channels + 2] =
                static_cast<uint8_t>( sum / std::max( 1, count ) );
        }
    }
}

void LightSnapshot::commit()
{
    std::lock_guard<std::mutex> lock( mutex_ );
    published_ = pending_;
    w_ = pending_w_;
    h_ = pending_h_;
    ++generation_;
}

godot::Ref<godot::Image> LightSnapshot::copy_image() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    if( w_ <= 0 || h_ <= 0 ||
        published_.size() != static_cast<size_t>( w_ ) * h_ * channels ) {
        return {};
    }
    godot::PackedByteArray bytes;
    bytes.resize( static_cast<int64_t>( published_.size() ) );
    std::memcpy( bytes.ptrw(), published_.data(), published_.size() );
    return godot::Image::create_from_data( w_, h_, false, godot::Image::FORMAT_RGBA8, bytes );
}

godot::Vector2i LightSnapshot::size() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return godot::Vector2i( w_, h_ );
}

uint64_t LightSnapshot::generation() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return generation_;
}

void LightSnapshot::set_pass_enabled( const bool on )
{
    pass_enabled_.store( on, std::memory_order_relaxed );
}

bool LightSnapshot::pass_enabled() const
{
    return pass_enabled_.load( std::memory_order_relaxed );
}

void LightSnapshot::set_wind( const float direction_deg, const float speed_mph )
{
    // A gale is around 40 mph; past that the shader has nothing more to give,
    // and foliage bent double looks worse than foliage bent hard.
    const float strength = std::clamp( speed_mph / 30.0f, 0.0f, 1.0f );
    const float rad = direction_deg * 3.14159265f / 180.0f;
    std::lock_guard<std::mutex> lock( mutex_ );
    // Compass degrees to screen space: 0 is north, and north is -y.
    wind_x_ = std::sin( rad ) * strength;
    wind_y_ = -std::cos( rad ) * strength;
}

godot::Vector2 LightSnapshot::wind() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return godot::Vector2( wind_x_, wind_y_ );
}

} // namespace godot_backend

#endif // GODOT
