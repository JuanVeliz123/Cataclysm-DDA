#if defined(GODOT)

#include "godot_pixel_minimap.h"
#include "godot_backend.h"
#include "cursesdef.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <godot_cpp/classes/time.hpp>

#include "avatar.h"
#include "cached_options.h"
#include "cata_assert.h"
#include "cata_utility.h"
#include "character.h"
#include "options.h"
#include "color.h"
#include "creature.h"
#include "creature_tracker.h"
#include "debug.h"
#include "game.h"
#include "level_cache.h"
#include "lightmap.h"
#include "map.h"
#include "map_scale_constants.h"
#include "mapdata.h"
#include "math_defines.h"
#include "mdarray.h"
#include "monster.h"
#include "mtype.h"
#include "type_id.h"
#include "vehicle.h"
#include "viewer.h"
#include "vpart_position.h"

namespace godot_backend
{

namespace
{

const point total_tiles_count = { MAX_VIEW_DISTANCE * 2 + 1, MAX_VIEW_DISTANCE * 2 + 1 };

tripoint_abs_sm center_to_abs_sm( const tripoint_bub_ms &center )
{
    return get_map().get_abs_sub() + rebase_rel( coords::project_to<coords::sm>( center ) );
}

// Enemy-beacon flicker sweeps brightness between these two percentages.
constexpr int beacon_flicker_dim = 25;
constexpr int beacon_flicker_full = 100;
// Milliseconds contributed by one unit of the beacon_blink_interval setting.
constexpr int beacon_blink_ms_per_step = 200;
// Edge pixels of the beacon diamond are darkened by this divisor to outline it.
constexpr int beacon_edge_divisor = 3;

point get_pixel_size( const point &tile_size, pixel_minimap_mode mode )
{
    switch( mode ) {
        case pixel_minimap_mode::solid:
            return tile_size;

        case pixel_minimap_mode::squares:
            return { std::max( tile_size.x - 1, 1 ), std::max( tile_size.y - 1, 1 ) };

        case pixel_minimap_mode::dots:
            return { point::south_east };
    }

    cata_fatal( "Invalid pixel_minimap_mode %d", static_cast<int>( mode ) );
}

/// Mirror of the same helper in cata_tiles.cpp, which is file-local there and in
/// a translation unit the Godot build compiles away.
pixel_minimap_mode pixel_minimap_mode_from_string( const std::string &mode )
{
    if( mode == "solid" ) {
        return pixel_minimap_mode::solid;
    } else if( mode == "squares" ) {
        return pixel_minimap_mode::squares;
    } else if( mode == "dots" ) {
        return pixel_minimap_mode::dots;
    }

    debugmsg( "Unsupported pixel minimap mode \"" + mode + "\"." );
    return pixel_minimap_mode::solid;
}

/// Returns a number in range [0..1]. The range lasts for @param phase_length_ms (milliseconds).
float get_animation_phase( int phase_length_ms )
{
    if( phase_length_ms == 0 ) {
        return 0.0f;
    }

    return std::fmod<float>( godot::Time::get_singleton()->get_ticks_msec(), phase_length_ms ) /
           phase_length_ms;
}

// --- Godot backend color helpers (analogs of sdl_utils.cpp) -----------------

bool is_black( const color &c )
{
    return
        c.r == 0x00 &&
        c.g == 0x00 &&
        c.b == 0x00;
}

uint8_t average_pixel_color( const color &c )
{
    return 85 * ( c.r + c.g + c.b ) >> 8; // 85/256 ~ 1/3
}

color adjust_color_brightness( const color &c, int percent )
{
    if( percent <= 0 ) {
        return { 0x00, 0x00, 0x00, c.a };
    }

    if( percent == 100 ) {
        return c;
    }

    return {
        static_cast<uint8_t>( std::min( c.r * percent / 100, 0xFF ) ),
        static_cast<uint8_t>( std::min( c.g * percent / 100, 0xFF ) ),
        static_cast<uint8_t>( std::min( c.b * percent / 100, 0xFF ) ),
        c.a
    };
}

color mix_colors( const color &first, const color &second, int second_percent )
{
    if( second_percent <= 0 ) {
        return first;
    }

    if( second_percent >= 100 ) {
        return second;
    }

    const int first_percent = 100 - second_percent;

    return {
        static_cast<uint8_t>( std::min( ( first.r * first_percent + second.r * second_percent ) / 100, 0xFF ) ),
        static_cast<uint8_t>( std::min( ( first.g * first_percent + second.g * second_percent ) / 100, 0xFF ) ),
        static_cast<uint8_t>( std::min( ( first.b * first_percent + second.b * second_percent ) / 100, 0xFF ) ),
        static_cast<uint8_t>( std::min( ( first.a * first_percent + second.a * second_percent ) / 100, 0xFF ) )
    };
}

color color_pixel_grayscale( const color &c )
{
    if( is_black( c ) ) {
        return c;
    }

    const uint8_t av = average_pixel_color( c );
    const uint8_t result = std::max( av * 5 >> 3, 0x01 );

    return { result, result, result, c.a };
}

color color_pixel_nightvision( const color &c )
{
    const uint8_t av = average_pixel_color( c );
    const uint8_t result = std::min( ( av * ( ( av * 3 >> 2 ) + 64 ) >> 8 ) + 16, 0xFF );

    return {
        static_cast<uint8_t>( result >> 2 ),
        static_cast<uint8_t>( result ),
        static_cast<uint8_t>( result >> 3 ),
        c.a
    };
}

color color_pixel_overexposed( const color &c )
{
    const uint8_t av = average_pixel_color( c );
    const uint8_t result = std::min( 64 + ( av * ( ( av >> 2 ) + 0xC0 ) >> 8 ), 0xFF );

    return {
        static_cast<uint8_t>( result >> 2 ),
        static_cast<uint8_t>( result ),
        static_cast<uint8_t>( result >> 3 ),
        c.a
    };
}

uint32_t pack_color( const color &c )
{
    return ( static_cast<uint32_t>( c.r ) << 24 ) |
           ( static_cast<uint32_t>( c.g ) << 16 ) |
           ( static_cast<uint32_t>( c.b ) << 8 ) |
           static_cast<uint32_t>( c.a );
}

color unpack_color( uint32_t v )
{
    return {
        static_cast<uint8_t>( v >> 24 ),
        static_cast<uint8_t>( v >> 16 ),
        static_cast<uint8_t>( v >> 8 ),
        static_cast<uint8_t>( v )
    };
}

// --- raw RGBA8 buffer helpers ----------------------------------------------

/// Write a single pixel into an RGBA8 buffer, silently clipping out-of-range coordinates.
void write_pixel( uint8_t *data, int width, int height, int x, int y, const color &c )
{
    if( x < 0 || x >= width || y < 0 || y >= height ) {
        return;
    }

    const size_t i = 4 * ( static_cast<size_t>( y ) * width + x );
    data[i] = c.r;
    data[i + 1] = c.g;
    data[i + 2] = c.b;
    data[i + 3] = c.a;
}

/// Fill a rectangle of an RGBA8 buffer, clipping to the buffer bounds.
void write_rect( uint8_t *data, int width, int height, int x, int y, int w, int h,
                 const color &c )
{
    const int x2 = std::min( x + w, width );
    const int y2 = std::min( y + h, height );
    x = std::max( x, 0 );
    y = std::max( y, 0 );

    for( int py = y; py < y2; ++py ) {
        size_t i = 4 * ( static_cast<size_t>( py ) * width + x );
        for( int px = x; px < x2; ++px, i += 4 ) {
            data[i] = c.r;
            data[i + 1] = c.g;
            data[i + 2] = c.b;
            data[i + 3] = c.a;
        }
    }
}

/// Blit a (possibly scaled) rectangle of an RGBA8 source buffer into an RGBA8
/// destination buffer, clipping to the destination bounds. The source and
/// destination rectangles are mapped with nearest-neighbor scaling.
void blit_scaled( uint8_t *dst, int dst_w, int dst_h,
                  const uint8_t *src, int src_w, int /*src_h*/,
                  const pixel_minimap_rect &src_rect, const pixel_minimap_rect &dst_rect )
{
    if( src_rect.w <= 0 || src_rect.h <= 0 || dst_rect.w <= 0 || dst_rect.h <= 0 ) {
        return;
    }

    const int clip_x = std::max( dst_rect.x, 0 );
    const int clip_y = std::max( dst_rect.y, 0 );
    const int clip_x2 = std::min( dst_rect.x + dst_rect.w, dst_w );
    const int clip_y2 = std::min( dst_rect.y + dst_rect.h, dst_h );

    if( clip_x >= clip_x2 || clip_y >= clip_y2 ) {
        return;
    }

    const int src_x_max = src_rect.x + src_rect.w;
    const int src_y_max = src_rect.y + src_rect.h;

    for( int py = clip_y; py < clip_y2; ++py ) {
        const int src_y = src_rect.y + ( ( py - dst_rect.y ) * src_rect.h ) / dst_rect.h;
        if( src_y < src_rect.y || src_y >= src_y_max ) {
            continue;
        }

        const size_t src_row = 4 * ( static_cast<size_t>( src_y ) * src_w );
        const size_t dst_row = 4 * ( static_cast<size_t>( py ) * dst_w );

        for( int px = clip_x; px < clip_x2; ++px ) {
            const int src_x = src_rect.x + ( ( px - dst_rect.x ) * src_rect.w ) / dst_rect.w;
            if( src_x < src_rect.x || src_x >= src_x_max ) {
                continue;
            }

            const size_t si = src_row + 4 * src_x;
            const size_t di = dst_row + 4 * px;
            dst[di] = src[si];
            dst[di + 1] = src[si + 1];
            dst[di + 2] = src[si + 2];
            dst[di + 3] = src[si + 3];
        }
    }
}

pixel_minimap_rect fit_rect_inside( const pixel_minimap_rect &inner,
                                    const pixel_minimap_rect &outer )
{
    const float inner_ratio = static_cast<float>( inner.w ) / inner.h;
    const float outer_ratio = static_cast<float>( outer.w ) / outer.h;
    const float factor = inner_ratio > outer_ratio
                         ? static_cast<float>( outer.w ) / inner.w
                         : static_cast<float>( outer.h ) / inner.h;

    const int w = factor * inner.w;
    const int h = factor * inner.h;
    const point p( outer.x + ( outer.w - w ) / 2, outer.y + ( outer.h - h ) / 2 );

    return { p.x, p.y, w, h };
}

color get_map_color_at( const tripoint_bub_ms &p )
{
    const map &here = get_map();
    if( const optional_vpart_position vp = here.veh_at( p ) ) {
        const vpart_display vd = vp->vehicle().get_display_of_tile( vp->mount_pos() );
        return curses_color_to_color( vd.color );
    }

    if( const furn_id &furn = here.furn( p ) ) {
        return curses_color_to_color( furn->color() );
    }

    return curses_color_to_color( here.ter( p )->color() );
}

color get_critter_color( Creature *critter, int flicker, int mixture )
{
    color result = curses_color_to_color( critter->symbol_color() );

    if( const monster *m = dynamic_cast<monster *>( critter ) ) {
        // faction status (attacking or tracking) determines if red highlights get applied to creature
        const monster_attitude matt = m->attitude( &get_player_character() );

        if( ( MATT_ATTACK == matt || MATT_FOLLOW == matt ) &&
            !m->has_flag( mon_flag_APPEARS_NEUTRAL ) ) {
            const color red_pixel = { 0xFF, 0x00, 0x00, 0xFF };
            result = adjust_color_brightness( mix_colors( result, red_pixel, mixture ), flicker );
        }
    }

    return result;
}

} // namespace

// --- projectors (Godot port of pixel_minimap_projectors.cpp) ----------------

pixel_minimap_ortho_projector::pixel_minimap_ortho_projector(
    const point &total_tiles_count, const pixel_minimap_rect &max_screen_rect,
    bool square_pixels )
{
    tile_size.x = std::max( max_screen_rect.w / total_tiles_count.x, 1 );
    tile_size.y = std::max( max_screen_rect.h / total_tiles_count.y, 1 );

    if( square_pixels ) {
        tile_size.x = tile_size.y = std::min( tile_size.x, tile_size.y );
    }
}

point pixel_minimap_ortho_projector::get_tile_size() const
{
    return tile_size;
}

point pixel_minimap_ortho_projector::get_tiles_size( const point &tiles_count ) const
{
    return {
        tiles_count.x * tile_size.x,
        tiles_count.y * tile_size.y
    };
}

point pixel_minimap_ortho_projector::get_tile_pos( const point &p,
        const point &/*tiles_count*/ ) const
{
    return { p.x * tile_size.x, p.y * tile_size.y };
}

pixel_minimap_rect pixel_minimap_ortho_projector::get_chunk_rect(
    const point &p, const point &tiles_count ) const
{
    return {
        p.x * tile_size.x,
        p.y * tile_size.y,
        tiles_count.x * tile_size.x,
        tiles_count.y * tile_size.y
    };
}

pixel_minimap_iso_projector::pixel_minimap_iso_projector(
    const point &total_tiles_count, const pixel_minimap_rect &max_screen_rect,
    bool square_pixels ) :
    total_tiles_count( total_tiles_count )
{
    tile_size.x = std::max( max_screen_rect.w / ( 2 * total_tiles_count.x - 1 ), 2 );
    tile_size.y = std::max( max_screen_rect.h / total_tiles_count.y, 2 );

    if( square_pixels ) {
        tile_size.x = tile_size.y = std::min( tile_size.x, tile_size.y );
    }
}

point pixel_minimap_iso_projector::get_tile_size() const
{
    return tile_size;
}

point pixel_minimap_iso_projector::get_tiles_size( const point &tiles_count ) const
{
    return {
        tile_size.x * ( 2 * tiles_count.x - 1 ),
        tile_size.y * tiles_count.y
    };
}

point pixel_minimap_iso_projector::get_tile_pos( const point &p,
        const point &tiles_count ) const
{
    return {
        tile_size.x * ( p.x + p.y ),
        tile_size.y * ( tiles_count.y + p.y - p.x - 1 ) / 2,
    };
}

pixel_minimap_rect pixel_minimap_iso_projector::get_chunk_rect(
    const point &p, const point &tiles_count ) const
{
    const point size = get_tiles_size( tiles_count );
    const point offset = point{ 0, tile_size.y * tiles_count.y / 2 };
    const point pos = get_tile_pos( p, total_tiles_count ) - offset;

    return { pos.x, pos.y, size.x, size.y };
}

// --- GodotPixelMinimap ------------------------------------------------------

// reserve the SEEX * SEEY submap tiles
GodotPixelMinimap::submap_cache::submap_cache( const point &chunk_size )
{
    minimap_colors.resize( SEEX * SEEY, 0 );

    if( chunk_size.x > 0 && chunk_size.y > 0 ) {
        chunk_pixels.assign( static_cast<size_t>( chunk_size.x ) * chunk_size.y * 4, 0 );
    }
}

uint32_t &GodotPixelMinimap::submap_cache::color_at( const point &p )
{
    cata_assert( p.x >= 0 && p.x < SEEX );
    cata_assert( p.y >= 0 && p.y < SEEY );

    return minimap_colors[p.y * SEEX + p.x];
}

const uint32_t &GodotPixelMinimap::submap_cache::color_at( const point &p ) const
{
    cata_assert( p.x >= 0 && p.x < SEEX );
    cata_assert( p.y >= 0 && p.y < SEEY );

    return minimap_colors[p.y * SEEX + p.x];
}

GodotPixelMinimap::GodotPixelMinimap() = default;

GodotPixelMinimap::~GodotPixelMinimap() = default;

void GodotPixelMinimap::set_type( pixel_minimap_type type )
{
    if( this->type != type ) {
        this->type = type;
        reset();
    }
}

void GodotPixelMinimap::set_settings( const pixel_minimap_settings &settings )
{
    this->settings = settings;
    reset();
}

void GodotPixelMinimap::prepare_cache_for_updates( const tripoint_bub_ms &center )
{
    const tripoint_abs_sm new_center_sm = center_to_abs_sm( center );
    const tripoint_rel_sm center_sm_diff = cached_center_sm - new_center_sm;

    // invalidate the cache if the game shifted more than one submap in the last update,
    // or if z-level changed.
    if( std::abs( center_sm_diff.x() ) > 1 ||
        std::abs( center_sm_diff.y() ) > 1 ||
        std::abs( center_sm_diff.z() ) > 0 ) {
        cache.clear();
    } else {
        for( auto &mcp : cache ) {
            mcp.second.touched = false;
        }
    }

    cached_center_sm = new_center_sm;
}

// deletes the mapping of unused submap caches from the main map
// the touched flag prevents deletion
void GodotPixelMinimap::clear_unused_cache()
{
    for( auto it = cache.begin(); it != cache.end(); ) {
        it = it->second.touched ? std::next( it ) : cache.erase( it );
    }
}

// draws individual updates to the submap cache image
void GodotPixelMinimap::flush_cache_updates()
{
    for( auto &mcp : cache ) {
        if( mcp.second.update_list.empty() ) {
            continue;
        }

        if( mcp.second.chunk_pixels.empty() ) {
            // no pixel buffer backing the chunk; drop the updates and skip
            mcp.second.update_list.clear();
            continue;
        }

        const int width = chunk_size.x;
        const int height = chunk_size.y;

        uint8_t *data = mcp.second.chunk_pixels.data();

        if( !mcp.second.ready ) {
            mcp.second.ready = true;

            std::fill( data, data + width * height * 4, 0x00 );
        }

        for( const point &p : mcp.second.update_list ) {
            const point tile_pos = projector->get_tile_pos( p, { SEEX, SEEY } );
            const color tile_color = unpack_color( mcp.second.color_at( p ) );

            if( pixel_size.x == 1 && pixel_size.y == 1 ) {
                write_pixel( data, width, height, tile_pos.x, tile_pos.y, tile_color );
            } else {
                write_rect( data, width, height, tile_pos.x, tile_pos.y,
                            pixel_size.x, pixel_size.y, tile_color );
            }
        }

        mcp.second.update_list.clear();
    }
}

void GodotPixelMinimap::update_cache_at( const tripoint_bub_sm &sm_pos )
{
    const map &here = get_map();
    const level_cache &access_cache = here.access_cache( sm_pos.z() );
    const bool nv_goggle = get_player_character().get_vision_modes()[NV_GOGGLES];

    submap_cache &cache_item = get_cache_at( here.get_abs_sub() + rebase_rel( sm_pos ) );
    const tripoint_bub_ms ms_pos = coords::project_to<coords::ms>( sm_pos );

    cache_item.touched = true;

    for( int y = 0; y < SEEY; ++y ) {
        for( int x = 0; x < SEEX; ++x ) {
            const tripoint_bub_ms p = ms_pos + tripoint{x, y, 0};
            const lit_level lighting = access_cache.visibility_cache[p.x()][p.y()];

            color color;

            if( lighting == lit_level::BLANK || lighting == lit_level::DARK ) {
                // TODO: Map memory?
                color = { static_cast<uint8_t>( pixel_minimap_r ),
                          static_cast<uint8_t>( pixel_minimap_g ),
                          static_cast<uint8_t>( pixel_minimap_b ),
                          static_cast<uint8_t>( pixel_minimap_a ) };
            } else {
                color = get_map_color_at( p );

                // color terrain according to lighting conditions
                if( nv_goggle ) {
                    if( lighting == lit_level::LOW ) {
                        color = color_pixel_nightvision( color );
                    } else if( lighting != lit_level::DARK && lighting != lit_level::BLANK ) {
                        color = color_pixel_overexposed( color );
                    }
                } else if( lighting == lit_level::LOW ) {
                    color = color_pixel_grayscale( color );
                }

                color = adjust_color_brightness( color, settings.brightness );
            }

            uint32_t &current_color = cache_item.color_at( { x, y } );

            if( current_color != pack_color( color ) ) {
                current_color = pack_color( color );
                cache_item.update_list.emplace_back( x, y );
            }
        }
    }
}

GodotPixelMinimap::submap_cache &GodotPixelMinimap::get_cache_at(
    const tripoint_abs_sm &abs_sm_pos )
{
    auto it = cache.find( abs_sm_pos );

    if( it == cache.end() ) {
        it = cache.emplace( abs_sm_pos, submap_cache( chunk_size ) ).first;
    }

    return it->second;
}

void GodotPixelMinimap::process_cache( const tripoint_bub_ms &center )
{
    prepare_cache_for_updates( center );

    for( int y = 0; y < MAPSIZE; ++y ) {
        for( int x = 0; x < MAPSIZE; ++x ) {
            update_cache_at( { x, y, center.z() } );
        }
    }

    flush_cache_updates();
    clear_unused_cache();
}

void GodotPixelMinimap::set_screen_rect( const pixel_minimap_rect &new_screen_rect )
{
    if( this->screen_rect.x == new_screen_rect.x &&
        this->screen_rect.y == new_screen_rect.y &&
        this->screen_rect.w == new_screen_rect.w &&
        this->screen_rect.h == new_screen_rect.h &&
        !main_pixels.empty() && projector ) {
        return;
    }

    this->screen_rect = new_screen_rect;

    projector = create_projector( new_screen_rect );
    pixel_size = get_pixel_size( projector->get_tile_size(), settings.mode );

    const point size_on_screen = projector->get_tiles_size( total_tiles_count );

    if( settings.scale_to_fit ) {
        main_tex_clip_rect = { 0, 0, size_on_screen.x, size_on_screen.y };
        screen_clip_rect = fit_rect_inside( main_tex_clip_rect, new_screen_rect );

    } else {
        const point d( ( size_on_screen.x - new_screen_rect.w ) / 2,
                       ( size_on_screen.y - new_screen_rect.h ) / 2 );

        main_tex_clip_rect = {
            std::max( d.x, 0 ),
            std::max( d.y, 0 ),
            size_on_screen.x - 2 * std::max( d.x, 0 ),
            size_on_screen.y - 2 * std::max( d.y, 0 )
        };

        screen_clip_rect = {
            new_screen_rect.x - std::min( d.x, 0 ),
            new_screen_rect.y - std::min( d.y, 0 ),
            main_tex_clip_rect.w,
            main_tex_clip_rect.h
        };
    }

    main_size = size_on_screen;
    main_pixels.assign( static_cast<size_t>( std::max( 0, size_on_screen.x ) ) *
                        std::max( 0, size_on_screen.y ) * 4, 0 );

    cache.clear();

    chunk_size = projector->get_tiles_size( { SEEX, SEEY } );
}

void GodotPixelMinimap::reset()
{
    projector.reset();
    cache.clear();
    main_pixels.clear();
    main_size = point::zero;
}

void GodotPixelMinimap::render( const tripoint_bub_ms &center )
{
    if( main_pixels.empty() ) {
        return;
    }

    const int width = main_size.x;
    const int height = main_size.y;
    uint8_t *data = main_pixels.data();

    // background fill, equivalent of the SDL version's RenderClear
    write_rect( data, width, height, 0, 0, width, height,
                { static_cast<uint8_t>( pixel_minimap_r ),
                  static_cast<uint8_t>( pixel_minimap_g ),
                  static_cast<uint8_t>( pixel_minimap_b ),
                  static_cast<uint8_t>( pixel_minimap_a ) } );

    render_cache( center, data, width, height );
    render_critters( center, data, width, height );

    // Publish the finished frame. Cropping to main_tex_clip_rect here rather than
    // in Godot keeps scale_to_fit / centring behaviour where the projector already
    // computed it, and hands out only the region that should be visible.
    const pixel_minimap_rect &clip = main_tex_clip_rect;
    const int out_w = std::clamp( clip.w, 0, width );
    const int out_h = std::clamp( clip.h, 0, height );
    const int off_x = std::clamp( clip.x, 0, std::max( 0, width - out_w ) );
    const int off_y = std::clamp( clip.y, 0, std::max( 0, height - out_h ) );

    std::vector<uint8_t> out;
    out.resize( static_cast<size_t>( out_w ) * out_h * 4 );
    for( int y = 0; y < out_h; ++y ) {
        const uint8_t *src = data + ( static_cast<size_t>( off_y + y ) * width + off_x ) * 4;
        std::copy( src, src + static_cast<size_t>( out_w ) * 4,
                   out.begin() + static_cast<size_t>( y ) * out_w * 4 );
    }

    std::lock_guard<std::mutex> lock( out_mutex_ );
    out_pixels_ = std::move( out );
    out_size_ = point( out_w, out_h );
    ++generation_;
}

std::vector<uint8_t> GodotPixelMinimap::copy_rgba( int &width, int &height ) const
{
    std::lock_guard<std::mutex> lock( out_mutex_ );
    width = out_size_.x;
    height = out_size_.y;
    return out_pixels_;
}

uint64_t GodotPixelMinimap::generation() const
{
    std::lock_guard<std::mutex> lock( out_mutex_ );
    return generation_;
}

void GodotPixelMinimap::render_cache( const tripoint_bub_ms &center,
                                      uint8_t *data, int width, int height )
{
    const tripoint_abs_sm sm_center = center_to_abs_sm( center );
    const tripoint_rel_sm sm_offset {
        total_tiles_count.x / SEEX / 2,
        total_tiles_count.y / SEEY / 2, 0
    };

    point_rel_ms ms_offset;
    tripoint_bub_sm quotient;
    point_sm_ms remainder;
    std::tie( quotient, remainder ) = coords::project_remain<coords::sm>( center );

    point_sm_ms ms_base_offset = point_sm_ms( ( total_tiles_count.x / 2 ) % SEEX,
                                 ( total_tiles_count.y / 2 ) % SEEY );
    ms_offset = ms_base_offset - remainder;

    for( const auto &elem : cache ) {
        if( !elem.second.touched ) {
            continue;   // What you gonna do with all that junk?
        }

        const tripoint_rel_sm rel_pos = elem.first - sm_center;

        if( std::abs( rel_pos.x() ) > sm_offset.x() + 1 ||
            std::abs( rel_pos.y() ) > sm_offset.y() + 1 ||
            rel_pos.z() != 0 ) {
            continue;
        }

        const tripoint_rel_sm sm_pos = tripoint_rel_sm( rel_pos ) + sm_offset;
        const tripoint_rel_ms ms_pos = coords::project_to<coords::ms>( sm_pos ) + ms_offset;

        if( elem.second.chunk_pixels.empty() ) {
            continue;
        }

        const pixel_minimap_rect chunk_rect = projector->get_chunk_rect( ms_pos.xy().raw(),
                { SEEX, SEEY } );

        const uint8_t *chunk_data = elem.second.chunk_pixels.data();
        const int chunk_w = chunk_size.x;
        const int chunk_h = chunk_size.y;
        const pixel_minimap_rect full_chunk_rect{ 0, 0, chunk_w, chunk_h };

        blit_scaled( data, width, height, chunk_data, chunk_w, chunk_h,
                     full_chunk_rect, chunk_rect );
    }
}

void GodotPixelMinimap::render_critters( const tripoint_bub_ms &center,
        uint8_t *data, int width, int height )
{
    has_blinking_beacons_ = false;

    const map &m = get_map();

    // handles the enemy faction red highlights
    // full blink period in milliseconds; default is 2000 ms, 2 seconds
    const int indicator_length = settings.beacon_blink_interval * beacon_blink_ms_per_step;

    int flicker = beacon_flicker_full;
    int mixture = 0;

    if( indicator_length > 0 ) {
        const float t = get_animation_phase( 2 * indicator_length );
        const float s = std::sin( 2 * M_PI * t );

        flicker = lerp_clamped( beacon_flicker_dim, beacon_flicker_full, std::abs( s ) );
        mixture = lerp_clamped( 0, beacon_flicker_full, std::max( s, 0.0f ) );
    }

    const level_cache &access_cache = m.access_cache( center.z() );

    const point_rel_ms start( center.x() - total_tiles_count.x / 2,
                              center.y() - total_tiles_count.y / 2 );
    const point beacon_size = {
        std::max<int>( projector->get_tile_size().x * settings.beacon_size / 2, 2 ),
        std::max<int>( projector->get_tile_size().y * settings.beacon_size / 2, 2 )
    };

    creature_tracker &creatures = get_creature_tracker();
    for( int y = 0; y < total_tiles_count.y; y++ ) {
        for( int x = 0; x < total_tiles_count.x; x++ ) {
            const tripoint_bub_ms p = start + tripoint_bub_ms( x, y, center.z() );
            if( !m.inbounds( p ) ) {
                // p might be out-of-bounds when peeking at submap boundary.
                continue;
            }
            const lit_level lighting = access_cache.visibility_cache[p.x()][p.y()];

            if( lighting == lit_level::DARK || lighting == lit_level::BLANK ) {
                continue;
            }

            Creature *critter = creatures.creature_at( p, true );

            if( critter == nullptr || !get_player_view().sees( m, *critter ) ) {
                continue;
            }

            const point critter_pos = projector->get_tile_pos( { x, y }, total_tiles_count );
            const pixel_minimap_rect critter_rect = pixel_minimap_rect{ critter_pos.x, critter_pos.y,
                    beacon_size.x, beacon_size.y };
            const color critter_color = get_critter_color( critter, flicker, mixture );

            if( indicator_length > 0 ) {
                has_blinking_beacons_ = true;
            }
            draw_beacon( critter_rect, critter_color, data, width, height );
        }
    }
}

// the main call for drawing the pixel minimap to the screen
void GodotPixelMinimap::draw( const point &size_px, const tripoint_bub_ms &center )
{
    if( !g ) {
        return;
    }

    if( size_px.x <= 0 || size_px.y <= 0 ) {
        return;
    }

    set_screen_rect( pixel_minimap_rect{ 0, 0, size_px.x, size_px.y } );

    if( main_pixels.empty() ) {
        // no compositing target available; skip the frame
        return;
    }

    process_cache( center );
    render( center );
}

void GodotPixelMinimap::draw_beacon( const pixel_minimap_rect &rect, const color &c,
                                     uint8_t *data, int width, int height )
{
    for( int x = -rect.w, x_max = rect.w; x <= x_max; ++x ) {
        for( int y = -rect.h + std::abs( x ), y_max = rect.h - std::abs( x ); y <= y_max; ++y ) {
            const bool on_edge = std::abs( y ) == rect.h - std::abs( x );
            const int divisor = on_edge ? beacon_edge_divisor : 1;

            write_pixel( data, width, height, rect.x + x, rect.y + y,
                         { static_cast<uint8_t>( c.r / divisor ),
                           static_cast<uint8_t>( c.g / divisor ),
                           static_cast<uint8_t>( c.b / divisor ), 0xFF } );
        }
    }
}

std::unique_ptr<pixel_minimap_projector> GodotPixelMinimap::create_projector(
    const pixel_minimap_rect &max_screen_rect ) const
{
    switch( type ) {
        case pixel_minimap_type::ortho:
            return std::make_unique<pixel_minimap_ortho_projector> ( total_tiles_count,
                    max_screen_rect, settings.square_pixels );

        case pixel_minimap_type::iso:
            return std::make_unique<pixel_minimap_iso_projector>( total_tiles_count,
                    max_screen_rect, settings.square_pixels );
    }

    cata_fatal( "Invalid pixel_minimap_type %d", static_cast<int>( type ) );
}

GodotPixelMinimap &get_pixel_minimap()
{
    static GodotPixelMinimap instance;
    return instance;
}

namespace
{
std::atomic<int> requested_w{0};
std::atomic<int> requested_h{0};
} // namespace

void set_pixel_minimap_size( const point &size_px )
{
    requested_w.store( std::max( 0, size_px.x ), std::memory_order_relaxed );
    requested_h.store( std::max( 0, size_px.y ), std::memory_order_relaxed );
}

void update_pixel_minimap()
{
    const point size( requested_w.load( std::memory_order_relaxed ),
                      requested_h.load( std::memory_order_relaxed ) );
    if( size.x <= 0 || size.y <= 0 ) {
        // Nobody is showing the panel; do not pay for a render.
        return;
    }
    if( !g ) {
        return;
    }

    GodotPixelMinimap &mm = get_pixel_minimap();

    // Same options the SDL path reads in cata_tiles.
    pixel_minimap_settings settings;
    settings.mode =
        pixel_minimap_mode_from_string( get_option<std::string>( "PIXEL_MINIMAP_MODE" ) );
    settings.brightness = get_option<int>( "PIXEL_MINIMAP_BRIGHTNESS" );
    settings.beacon_size = get_option<int>( "PIXEL_MINIMAP_BEACON_SIZE" );
    settings.beacon_blink_interval = get_option<int>( "PIXEL_MINIMAP_BLINK" );
    settings.square_pixels = get_option<bool>( "PIXEL_MINIMAP_RATIO" );
    settings.scale_to_fit = get_option<bool>( "PIXEL_MINIMAP_SCALE_TO_FIT" );
    mm.set_settings( settings );

    map &here = get_map();
    mm.draw( size, get_avatar().pos_bub( here ) );
}

} // namespace godot_backend

#endif // GODOT
