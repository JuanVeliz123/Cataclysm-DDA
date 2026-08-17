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
    pending_deepest_ = 0;
    // Room for every level a draw command can name, so the walk never has to know how
    // deep it will go before it goes there. Only the blocks it reaches are published.
    const size_t want = static_cast<size_t>( pending_w_ ) * pending_h_ * max_levels * channels;
    pending_.assign( want, 0 );
    // The light list belongs to the frame, not to the session: a lamp that went
    // out has to stop being published, and clearing here is what makes the walk
    // that fills it authoritative rather than additive.
    pending_lights_.clear();
}

/// Index of the texel for tile (@p x, @p y) on @p level, or npos when out of range.
size_t LightSnapshot::texel( const int x, const int y, const int level ) const
{
    if( x < 0 || y < 0 || x >= pending_w_ || y >= pending_h_ ||
        level < 0 || level >= max_levels ) {
        return npos;
    }
    const size_t row = static_cast<size_t>( level ) * pending_h_ + y;
    return ( row * pending_w_ + x ) * channels;
}

void LightSnapshot::set( const int x, const int y, const int level,
                         const uint8_t visibility, const uint8_t light )
{
    const size_t i = texel( x, y, level );
    if( i == npos ) {
        return;
    }
    pending_[i] = visibility;
    pending_[i + 1] = light;
    // Alpha is unused but must not be zero: an Image with a zero alpha channel is
    // legal, and every tool that ever looks at this texture would show it as empty.
    // Written per touched texel rather than for the whole buffer, which at sixteen
    // levels is most of a megabyte of nothing.
    pending_[i + 3] = 255;
    pending_deepest_ = std::max( pending_deepest_, level );
}

void LightSnapshot::set_fire( const int x, const int y, const int level,
                              const uint8_t fire )
{
    const size_t i = texel( x, y, level );
    if( i == npos ) {
        return;
    }
    pending_[i + 2] = fire;
    pending_deepest_ = std::max( pending_deepest_, level );
}

void LightSnapshot::add_light( const float x, const float y, const float radius,
                               const light_color_rgb &color, const float luminance,
                               const float bearing_deg, const float cone_deg )
{
    // A source with no reach is a source nothing can see, and one of those per
    // unlit tile would be most of the map.
    if( radius <= 0.0f || luminance <= 0.0f ) {
        return;
    }
    // light_color_rgb is accumulated *energy*, by its own documentation "not
    // display-ready" -- so its magnitude says nothing a renderer should read as a
    // colour. Normalising against the largest channel keeps the hue and throws the
    // magnitude away, which is what `luminance` is for. A source that declares no
    // colour at all -- which is most of them -- is white rather than black.
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    const float peak = std::max( { color.r, color.g, color.b } );
    if( color.is_colored() && peak > 0.0f ) {
        r = color.r / peak;
        g = color.g / peak;
        b = color.b / peak;
    }
    pending_lights_.insert( pending_lights_.end(),
    { x, y, radius, r, g, b, luminance, bearing_deg, cone_deg } );
}

void LightSnapshot::blur_fire()
{
    if( pending_w_ <= 0 || pending_h_ <= 0 ) {
        return;
    }
    // Per level, and never across one: a fire on the floor above is not a glow on the
    // ceiling of the room below, and the blocks are adjacent rows in one image, so a blur
    // that ignored the boundary would smear exactly that.
    const size_t plane = static_cast<size_t>( pending_w_ ) * pending_h_;
    std::vector<uint8_t> src( plane );
    for( int level = 0; level <= pending_deepest_ && level < max_levels; ++level ) {
        const size_t base = static_cast<size_t>( level ) * plane;
        bool any = false;
        for( size_t i = 0; i < plane; ++i ) {
            src[i] = pending_[( base + i ) * channels + 2];
            any = any || src[i] != 0;
        }
        if( !any ) {
            continue;
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
                pending_[( base + static_cast<size_t>( y ) * pending_w_ + x ) * channels + 2] =
                    static_cast<uint8_t>( sum / std::max( 1, count ) );
            }
        }
    }
}

void LightSnapshot::commit()
{
    std::lock_guard<std::mutex> lock( mutex_ );
    // Only the levels the walk reached. Outdoors that is one, and publishing sixteen
    // would be most of a megabyte of "never seen" per turn.
    levels_ = std::clamp( pending_deepest_ + 1, 1, max_levels );
    const size_t used = static_cast<size_t>( pending_w_ ) * pending_h_ * levels_ * channels;
    published_.assign( pending_.begin(),
                       pending_.begin() + static_cast<ptrdiff_t>( std::min( used, pending_.size() ) ) );
    published_lights_ = pending_lights_;
    w_ = pending_w_;
    h_ = pending_h_;
    ++generation_;
}

godot::Ref<godot::Image> LightSnapshot::copy_image() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    if( w_ <= 0 || h_ <= 0 ||
        published_.size() != static_cast<size_t>( w_ ) * h_ * levels_ * channels ) {
        return {};
    }
    godot::PackedByteArray bytes;
    bytes.resize( static_cast<int64_t>( published_.size() ) );
    std::memcpy( bytes.ptrw(), published_.data(), published_.size() );
    // One block of h rows per level; see max_levels. The shader is told how many.
    return godot::Image::create_from_data( w_, h_ * levels_, false,
                                          godot::Image::FORMAT_RGBA8, bytes );
}

godot::PackedFloat32Array LightSnapshot::copy_lights() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::PackedFloat32Array out;
    out.resize( static_cast<int64_t>( published_lights_.size() ) );
    if( !published_lights_.empty() ) {
        std::memcpy( out.ptrw(), published_lights_.data(),
                     published_lights_.size() * sizeof( float ) );
    }
    return out;
}

int LightSnapshot::levels() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return levels_;
}

int LightSnapshot::light_count() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return static_cast<int>( published_lights_.size() / light_stride );
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

void LightSnapshot::set_depth_fog_enabled( const bool on )
{
    depth_fog_enabled_.store( on, std::memory_order_relaxed );
}

bool LightSnapshot::depth_fog_enabled() const
{
    return depth_fog_enabled_.load( std::memory_order_relaxed );
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

void LightSnapshot::set_conditions( const conditions &c )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    conditions_ = c;
}

LightSnapshot::conditions LightSnapshot::get_conditions() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return conditions_;
}

godot::Vector2 LightSnapshot::wind() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return godot::Vector2( wind_x_, wind_y_ );
}

} // namespace godot_backend

#endif // GODOT
