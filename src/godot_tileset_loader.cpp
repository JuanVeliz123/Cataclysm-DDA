#include "godot_tileset_loader.h"

#if defined(GODOT)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cata_assert.h"
#include "cata_path.h"
#include "cata_utility.h"
#include "cursesdef.h"
#include "debug.h"
#include "flexbuffer_json.h"
#include "json_loader.h"
#include "mod_tileset.h"
#include "options.h"
#include "output.h"
#include "overlay_ordering.h"
#include "path_info.h"
#include "rect_range.h"
#include "type_id.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#define dbg(x) DebugLog((x),D_SDL) << __FILE__ << ":" << __LINE__ << ": "

static const std::string ITEM_HIGHLIGHT( "highlight_item" );

namespace godot_backend
{

namespace
{

std::string get_ascii_tile_id( const uint32_t sym, const int FG, const int BG )
{
    return std::string( { 'A', 'S', 'C', 'I', 'I', '_', static_cast<char>( sym ),
                          static_cast<char>( FG ), static_cast<char>( BG )
                        } );
}

/// Integer rect used in place of SDL_Rect by the Godot loader. Field order
/// matches the aggregate {x, y, w, h} that rect_range constructs.
struct pixel_rect {
    int x;
    int y;
    int w;
    int h;
};

/// RGBA pixel used by the CPU color filters below, matching Godot's RGBA8
/// byte order (R, G, B, A). Mirrors the SDL_Color filters in sdl_utils.cpp.
struct pixel {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 255;
};

inline bool is_black( const pixel &p )
{
    return p.r == 0x00 && p.g == 0x00 && p.b == 0x00;
}

inline uint8_t average_pixel_color( const pixel &p )
{
    return static_cast<uint8_t>( 85 * ( p.r + p.g + p.b ) >> 8 ); // 85/256 ~ 1/3
}

inline pixel mix_colors( const pixel &first, const pixel &second, int second_percent )
{
    if( second_percent <= 0 ) {
        return first;
    }
    if( second_percent >= 100 ) {
        return second;
    }
    const int first_percent = 100 - second_percent;
    const auto mix = [&]( uint8_t a, uint8_t b ) {
        return static_cast<uint8_t>( std::min( ( a * first_percent + b * second_percent ) / 100, 0xFF ) );
    };
    return pixel{ mix( first.r, second.r ), mix( first.g, second.g ),
                  mix( first.b, second.b ), mix( first.a, second.a ) };
}

inline pixel color_pixel_grayscale( const pixel &p )
{
    if( is_black( p ) ) {
        return p;
    }
    const uint8_t av = average_pixel_color( p );
    const uint8_t result = static_cast<uint8_t>( std::max( av * 5 >> 3, 0x01 ) );
    return pixel{ result, result, result, p.a };
}

inline pixel color_pixel_nightvision( const pixel &p )
{
    const uint8_t av = average_pixel_color( p );
    const uint8_t result = static_cast<uint8_t>(
                               std::min( ( av * ( ( av * 3 >> 2 ) + 64 ) >> 8 ) + 16, 0xFF ) );
    return pixel{ static_cast<uint8_t>( result >> 2 ), result,
                  static_cast<uint8_t>( result >> 3 ), p.a };
}

inline pixel color_pixel_overexposed( const pixel &p )
{
    const uint8_t av = average_pixel_color( p );
    const uint8_t result = static_cast<uint8_t>(
                               std::min( 64 + ( av * ( ( av >> 2 ) + 0xC0 ) >> 8 ), 0xFF ) );
    return pixel{ static_cast<uint8_t>( result >> 2 ), result,
                  static_cast<uint8_t>( result >> 3 ), p.a };
}

inline pixel color_pixel_darken( const pixel &p )
{
    if( is_black( p ) ) {
        return p;
    }
    // 85/256 ~ 1/3
    return pixel{ std::max<uint8_t>( 85 * p.r >> 8, 0x01 ),
                  std::max<uint8_t>( 85 * p.g >> 8, 0x01 ),
                  std::max<uint8_t>( 85 * p.b >> 8, 0x01 ), p.a };
}

inline pixel color_pixel_mixer( const pixel &p, const float gammav,
                                const pixel &color_a, const pixel &color_b )
{
    if( is_black( p ) ) {
        return p;
    }
    const uint8_t av = average_pixel_color( p );
    const float pv = av / 255.0f;
    const uint8_t finalv = static_cast<uint8_t>(
                               std::min( static_cast<int>( std::round( std::pow( pv, gammav ) * 150 ) ), 100 ) );
    return mix_colors( color_a, color_b, finalv );
}

inline pixel color_pixel_silhouette( const pixel &p )
{
    return pixel{ 255, 255, 255, p.a };
}

inline pixel color_pixel_sepia_light( const pixel &p )
{
    const pixel dark = pixel{ 39, 23, 19, p.a };
    const pixel light = pixel{ 241, 220, 163, p.a };
    return color_pixel_mixer( p, 1.6f, dark, light );
}

inline pixel color_pixel_sepia_dark( const pixel &p )
{
    const pixel dark = pixel{ 39, 23, 19, p.a };
    const pixel light = pixel{ 70, 66, 60, p.a };
    return color_pixel_mixer( p, 1.0f, dark, light );
}

inline pixel color_pixel_blue_dark( const pixel &p )
{
    const pixel dark = pixel{ 19, 23, 39, p.a };
    const pixel light = pixel{ 60, 66, 70, p.a };
    return color_pixel_mixer( p, 1.0f, dark, light );
}

inline pixel color_pixel_custom( const pixel &p )
{
    const pixel dark = pixel{ static_cast<uint8_t>( get_option<int>( "MEMORY_RGB_DARK_RED" ) ),
                              static_cast<uint8_t>( get_option<int>( "MEMORY_RGB_DARK_GREEN" ) ),
                              static_cast<uint8_t>( get_option<int>( "MEMORY_RGB_DARK_BLUE" ) ), p.a };
    const pixel light = pixel{ static_cast<uint8_t>( get_option<int>( "MEMORY_RGB_BRIGHT_RED" ) ),
                               static_cast<uint8_t>( get_option<int>( "MEMORY_RGB_BRIGHT_GREEN" ) ),
                               static_cast<uint8_t>( get_option<int>( "MEMORY_RGB_BRIGHT_BLUE" ) ), p.a };
    return color_pixel_mixer( p, get_option<float>( "MEMORY_GAMMA" ), dark, light );
}

using pixel_filter = pixel( * )( const pixel & );

pixel_filter get_color_pixel_function( const std::string &name )
{
    static const std::unordered_map<std::string, pixel_filter> builtin_color_pixel_functions = {
        { "color_pixel_none", nullptr },
        { "color_pixel_darken", color_pixel_darken },
        { "color_pixel_sepia_light", color_pixel_sepia_light },
        { "color_pixel_sepia_dark", color_pixel_sepia_dark },
        { "color_pixel_blue_dark", color_pixel_blue_dark },
        { "color_pixel_custom", color_pixel_custom },
        { "color_pixel_grayscale", color_pixel_grayscale },
        { "color_pixel_nightvision", color_pixel_nightvision },
        { "color_pixel_overexposed", color_pixel_overexposed },
        { "color_pixel_silhouette", color_pixel_silhouette },
    };
    const auto iter = builtin_color_pixel_functions.find( name );
    if( iter == builtin_color_pixel_functions.end() ) {
        debugmsg( "no color pixel function with name %s", name );
        return nullptr;
    }
    return iter->second;
}

/// Apply @p filter to every pixel of an RGBA8 byte vector, preserving alpha.
std::vector<uint8_t> apply_color_filter( const std::vector<uint8_t> &rgba,
        pixel_filter filter )
{
    std::vector<uint8_t> out = rgba;
    const size_t count = out.size() / 4;
    for( size_t i = 0; i < count; i++ ) {
        const size_t base = i * 4;
        if( out[base + 3] == 0x00 ) {
            // Vast majority of pixels in the tilesets are completely transparent.
            continue;
        }
        const pixel src{ out[base], out[base + 1], out[base + 2], out[base + 3] };
        const pixel dst = filter( src );
        out[base] = dst.r;
        out[base + 1] = dst.g;
        out[base + 2] = dst.b;
        out[base + 3] = dst.a;
    }
    return out;
}

/// Compute the tightest bounding box containing non-transparent pixels within
/// a sprite rect of an RGBA8 atlas. Returns coordinates relative to the sprite
/// rect origin. A fully transparent sprite returns {0, 0, 0, 0}.
pixel_rect compute_opaque_rect( const std::vector<uint8_t> &rgba, const int atlas_width,
                                const pixel_rect &rect )
{
    int min_x = rect.w;
    int min_y = rect.h;
    int max_x = -1;
    int max_y = -1;
    for( int y = 0; y < rect.h; y++ ) {
        for( int x = 0; x < rect.w; x++ ) {
            const size_t i = 4 * ( static_cast<size_t>( rect.y + y ) * atlas_width + rect.x + x );
            if( i + 3 < rgba.size() && rgba[i + 3] > 0 ) {
                if( x < min_x ) {
                    min_x = x;
                }
                if( y < min_y ) {
                    min_y = y;
                }
                if( x > max_x ) {
                    max_x = x;
                }
                if( y > max_y ) {
                    max_y = y;
                }
            }
        }
    }
    if( max_x < 0 ) {
        return pixel_rect{ 0, 0, 0, 0 };
    }
    return pixel_rect{ min_x, min_y, max_x - min_x + 1, max_y - min_y + 1 };
}

} // namespace

void godot_tileset::clear()
{
    tile_values.clear();
    atlas_textures.clear();
    duplicate_ids.clear();
    tile_ids.clear();
    item_layer_data.clear();
    field_layer_data.clear();
    default_item_highlight_index_.reset();
}

godot_tile_type &godot_tileset::create_tile_type( const std::string &id,
        godot_tile_type &&new_tile_type )
{
    return tile_ids.emplace( id, std::move( new_tile_type ) ).first->second;
}

const godot_tile_type *godot_tileset::find_tile_type( const std::string &id ) const
{
    const auto iter = tile_ids.find( id );
    return iter != tile_ids.end() ? &iter->second : nullptr;
}

namespace
{

void get_tile_information( const cata_path &config_path, std::string &json_path,
                           std::string &tileset_path, std::string &layering_path )
{
    const std::string default_json = PATH_INFO::defaulttilejson();
    const std::string default_tileset = PATH_INFO::defaulttilepng();
    const std::string default_layering = PATH_INFO::defaultlayeringjson();

    // Get JSON and TILESET vars from config
    const auto reader = [&]( std::istream & fin ) {
        while( !fin.eof() ) {
            std::string sOption;
            fin >> sOption;

            if( string_starts_with( sOption, "JSON" ) ) {
                fin >> json_path;
                dbg( D_INFO ) << "JSON path set to [" << json_path << "].";
            } else if( string_starts_with( sOption, "TILESET" ) ) {
                fin >> tileset_path;
                dbg( D_INFO ) << "TILESET path set to [" << tileset_path << "].";
            } else if( string_starts_with( sOption, "LAYERING" ) ) {
                fin >> layering_path;
                dbg( D_INFO ) << "LAYERING path set to [" << layering_path << "].";

            } else {
                getline( fin, sOption );
            }
        }
    };

    if( !read_from_file( config_path, reader ) ) {
        json_path = default_json;
        tileset_path = default_tileset;
        layering_path = default_layering;
    }

    if( json_path.empty() ) {
        json_path = default_json;
        dbg( D_INFO ) << "JSON set to default [" << json_path << "].";
    }
    if( tileset_path.empty() ) {
        tileset_path = default_tileset;
        dbg( D_INFO ) << "TILESET set to default [" << tileset_path << "].";
    }
    if( layering_path.empty() ) {
        layering_path = default_layering;
        dbg( D_INFO ) << "LAYERING set to default [" << layering_path << "].";
    }
}

/// Load an atlas PNG into a decoded RGBA8 godot::Image. Throws on failure.
godot::Ref<godot::Image> load_atlas_image( const cata_path &img_path )
{
    const godot::String path( img_path.get_unrelative_path().u8string().c_str() );
    godot::Ref<godot::Image> atlas = godot::Image::load_from_file( path );
    if( !atlas.is_valid() ) {
        throw std::runtime_error( "Failed to load tileset image: " +
                                  img_path.get_unrelative_path().u8string() );
    }
    // Guarantee a known byte layout (R, G, B, A) for the CPU filters below.
    atlas->convert( godot::Image::FORMAT_RGBA8 );
    return atlas;
}

/// Copy the RGBA8 pixels of @p image into a std::vector, applying the
/// transparency color key (all-negative disables keying).
std::vector<uint8_t> image_to_rgba( const godot::Ref<godot::Image> &image,
                                    const int kr, const int kg, const int kb )
{
    const godot::PackedByteArray data = image->get_data();
    std::vector<uint8_t> rgba( data.size() );
    std::memcpy( rgba.data(), data.ptr(), data.size() );

    if( kr >= 0 && kr <= 255 && kg >= 0 && kg <= 255 && kb >= 0 && kb <= 255 ) {
        const size_t count = rgba.size() / 4;
        for( size_t i = 0; i < count; i++ ) {
            const size_t base = i * 4;
            if( rgba[base] == static_cast<uint8_t>( kr ) &&
                rgba[base + 1] == static_cast<uint8_t>( kg ) &&
                rgba[base + 2] == static_cast<uint8_t>( kb ) ) {
                rgba[base + 3] = 0x00;
            }
        }
    }
    return rgba;
}

godot::Ref<godot::ImageTexture> make_atlas_texture( const godot::Ref<godot::Image> &image,
        const std::vector<uint8_t> &rgba )
{
    godot::PackedByteArray bytes;
    bytes.resize( static_cast<int64_t>( rgba.size() ) );
    std::memcpy( bytes.ptrw(), rgba.data(), rgba.size() );
    godot::Ref<godot::Image> filtered = godot::Image::create_from_data(
                                            image->get_width(), image->get_height(), false,
                                            godot::Image::FORMAT_RGBA8, bytes );
    return godot::ImageTexture::create_from_image( filtered );
}

} // namespace

void godot_tileset_loader::load( godot_tileset &result, const std::string &tileset_id,
                                 const bool precheck, const bool pump_events, const bool terrain )
{
    ( void )pump_events; // Godot input bridge (T3.1) is not wired yet.

    ts = &result;
    atlases_.clear();

    std::string json_conf;
    std::string layering;
    std::string tileset_path;
    cata_path tileset_root;

    bool has_layering = true;

    const auto tset_iter = TILESETS.find( tileset_id );
    if( tset_iter != TILESETS.end() ) {
        tileset_root = tset_iter->second;
        dbg( D_INFO ) << '"' << tileset_id << '"' << " tileset: found config file path: " <<
                      tileset_root;
        get_tile_information( tileset_root / PATH_INFO::tileset_conf(),
                              json_conf, tileset_path, layering );
        dbg( D_INFO ) << "Current tileset is: " << tileset_id;
    } else {
        dbg( D_ERROR ) << "Tileset \"" << tileset_id << "\" from options is invalid";
        json_conf = PATH_INFO::defaulttilejson();
        tileset_path = PATH_INFO::defaulttilepng();
        layering = PATH_INFO::defaultlayeringjson();
    }

    cata_path json_path = tileset_root / std::filesystem::u8path( json_conf );
    cata_path img_path = tileset_root / std::filesystem::u8path( tileset_path );
    cata_path layering_path = tileset_root / std::filesystem::u8path( layering );

    dbg( D_INFO ) << "Attempting to Load LAYERING file " << layering_path;
    std::ifstream layering_file( layering_path.get_unrelative_path(),
                                 std::ifstream::in | std::ifstream::binary );

    if( !layering_file.good() ) {
        has_layering = false;
    }

    dbg( D_INFO ) << "Attempting to Load JSON file " << json_path;
    std::optional<JsonValue> config_json = json_loader::from_path_opt( json_path );

    if( !config_json.has_value() ) {
        throw std::runtime_error( std::string( "Failed to open tile info json: " ) +
                                  json_path.generic_u8string() );
    }

    JsonObject config = ( *config_json ).get_object();
    config.allow_omitted_members();

    // "tile_info" section must exist.
    if( !config.has_member( "tile_info" ) ) {
        config.throw_error( "\"tile_info\" missing" );
    }

    for( const JsonObject curr_info : config.get_array( "tile_info" ) ) {
        ts->tile_height_ = curr_info.get_int( "height" );
        ts->tile_width_ = curr_info.get_int( "width" );
        ts->max_tile_extent_ = half_open_rectangle<point>( point::zero,
                               { ts->tile_width_, ts->tile_height_ } );
        ts->zlevel_height_ = curr_info.get_int( "zlevel_height", 0 );
        ts->tile_isometric_ = curr_info.get_bool( "iso", false );
        ts->tile_pixelscale_ = curr_info.get_float( "pixelscale", 1.0f );
        ts->prevent_occlusion_min_dist_ = curr_info.get_float( "retract_dist_min", -1.0f );
        ts->prevent_occlusion_max_dist_ = curr_info.get_float( "retract_dist_max", 0.0f );
    }

    if( precheck ) {
        // A precheck parses metadata only; no textures are loaded.
        return;
    }

    ts->clear();

    // Parse pass: record atlas descriptors and tile mappings, no GPU work.
    // Texture upload happens later in upload_atlases.
    offset = 0;
    parse_atlases( config, tileset_root, img_path, pump_events );

    // Load mod tilesets if available
    for( const mod_tileset &mts : all_mod_tilesets ) {
        // Set sprite_id offset to separate from other tilesets.
        sprite_id_offset = offset;
        tileset_root = mts.get_base_path();
        json_path = mts.get_full_path();

        if( !mts.is_compatible( tileset_id ) ) {
            dbg( D_ERROR ) << "Mod tileset in \"" << json_path << "\" is not compatible with \""
                           << tileset_id << "\".";
            continue;
        }
        dbg( D_INFO ) << "Attempting to Load JSON file " << json_path;
        std::optional<JsonValue> mod_config_json_opt = json_loader::from_path_opt( json_path );

        if( !mod_config_json_opt.has_value() ) {
            throw std::runtime_error( std::string( "Failed to open tile info json: " ) +
                                      json_path.generic_u8string() );
        }

        JsonValue &mod_config_json = *mod_config_json_opt;

        const auto mark_visited = []( const JsonObject & jobj ) {
            // These fields have been visited in load_mod_tileset
            jobj.get_string_array( "compatibility" );
        };

        int num_in_file = 1;
        if( mod_config_json.test_array() ) {
            for( const JsonObject mod_config : mod_config_json.get_array() ) {
                if( mod_config.get_string( "type" ) == "mod_tileset" ) {
                    mark_visited( mod_config );
                    if( num_in_file == mts.num_in_file() ) {
                        // visit this if it exists, it's used elsewhere
                        if( mod_config.has_member( "compatibility" ) ) {
                            mod_config.get_member( "compatibility" );
                        }
                        parse_atlases( mod_config, tileset_root, img_path, pump_events );
                        break;
                    }
                    num_in_file++;
                }
            }
        } else {
            JsonObject mod_config = mod_config_json.get_object();
            mark_visited( mod_config );
            parse_atlases( mod_config, tileset_root, img_path, pump_events );
        }
    }

    // loop through all tile ids and eliminate empty/invalid things
    for( auto it = ts->tile_ids.begin(); it != ts->tile_ids.end(); ) {
        // second is the tile_type describing that id
        godot_tile_type &td = it->second;
        process_variations_after_loading( td.fg );
        process_variations_after_loading( td.bg );
        // All tiles need at least foreground or background data, otherwise they are useless.
        if( td.bg.empty() && td.fg.empty() ) {
            dbg( D_ERROR ) << "tile " << it->first << " has no (valid) foreground nor background";
            ts->tile_ids.erase( it++ );
        } else {
            ++it;
        }
    }

    if( !ts->find_tile_type( terrain ? "unknown_terrain" : "unknown" ) ) {
        dbg( D_ERROR ) << "The tileset you're using has no '"
                       << ( terrain ? "unknown_terrain" : "unknown" ) << "' tile defined!";
    }

    // When the tileset lacks ITEM_HIGHLIGHT, reserve the next free slot for
    // the synthetic overlay. upload_atlases fills it on every upload, so the
    // slot stays stable across reloads.
    if( !ts->find_tile_type( ITEM_HIGHLIGHT ) ) {
        const int reserved_index = offset;
        ts->set_default_item_highlight_index( reserved_index );
        ts->tile_ids[ITEM_HIGHLIGHT].fg.add( std::vector<int>( {reserved_index} ), 1 );
    } else {
        ts->set_default_item_highlight_index( std::nullopt );
    }

    ts->tileset_id_ = tileset_id;

    // set up layering data
    if( has_layering ) {
        JsonValue layering_json = json_loader::from_path( layering_path );
        JsonObject layer_config = layering_json.get_object();
        layer_config.allow_omitted_members();

        // "variants" section must exist.
        if( !layer_config.has_member( "variants" ) ) {
            layer_config.throw_error( "\"variants\" missing" );
        }

        load_layers( layer_config );
    }

    load_palettes( tileset_root );

    upload_atlases();
}

/// Parse "#rrggbb" (or "rrggbb"). Returns false on anything else, so a typo in
/// the palette file drops one entry rather than the whole ramp.
static bool parse_hex_rgb( const std::string &text, std::array<uint8_t, 3> &out )
{
    const std::string hex = !text.empty() && text[0] == '#' ? text.substr( 1 ) : text;
    if( hex.size() != 6 ) {
        return false;
    }
    for( const char c : hex ) {
        if( !std::isxdigit( static_cast<unsigned char>( c ) ) ) {
            return false;
        }
    }
    const unsigned long v = std::stoul( hex, nullptr, 16 );
    out = { static_cast<uint8_t>( ( v >> 16 ) & 0xFF ),
            static_cast<uint8_t>( ( v >> 8 ) & 0xFF ),
            static_cast<uint8_t>( v & 0xFF )
          };
    return true;
}

void godot_tileset_loader::load_palettes( const cata_path &tileset_root )
{
    ts->palettes.clear();
    ts->sprite_variants.clear();

    // A tileset may ship its own; otherwise the shared file in data/godot/
    // applies to every tileset, which is what makes a palette variant work
    // against art the tileset author never thought about.
    cata_path path = tileset_root / "godot_palettes.json";
    std::optional<JsonValue> json = json_loader::from_path_opt( path );
    if( !json.has_value() ) {
        path = PATH_INFO::datadir_path() / "godot" / "palettes.json";
        json = json_loader::from_path_opt( path );
    }
    if( !json.has_value() ) {
        return;
    }

    JsonObject root = ( *json ).get_object();
    root.allow_omitted_members();

    std::unordered_map<std::string, int> row_of;
    if( root.has_array( "palettes" ) ) {
        for( JsonObject entry : root.get_array( "palettes" ) ) {
            // The file documents itself with "//" members, which the reader
            // would otherwise report as unvisited.
            entry.allow_omitted_members();
            godot_palette pal;
            pal.id = entry.get_string( "id", "" );
            for( const std::string &swatch : entry.get_string_array( "ramp" ) ) {
                std::array<uint8_t, 3> rgb{};
                if( parse_hex_rgb( swatch, rgb ) ) {
                    pal.ramp.push_back( rgb );
                } else {
                    dbg( D_ERROR ) << "palette " << pal.id << ": bad colour \"" << swatch << "\"";
                }
            }
            if( pal.id.empty() || pal.ramp.empty() ) {
                continue;
            }
            // Rows are 1-based: the draw command packs 0 as "no palette", and
            // that has to stay distinguishable from the first palette.
            row_of[pal.id] = static_cast<int>( ts->palettes.size() ) + 1;
            ts->palettes.push_back( std::move( pal ) );
        }
    }

    if( root.has_array( "variants" ) ) {
        for( JsonObject entry : root.get_array( "variants" ) ) {
            entry.allow_omitted_members();
            const std::string id = entry.get_string( "id", "" );
            if( id.empty() ) {
                continue;
            }
            godot_sprite_variant var;
            var.sprite = entry.get_string( "sprite", "" );
            const std::string pal = entry.get_string( "palette", "" );
            if( !pal.empty() ) {
                const auto it = row_of.find( pal );
                if( it == row_of.end() ) {
                    dbg( D_ERROR ) << "variant " << id << ": unknown palette \"" << pal << "\"";
                    continue;
                }
                var.palette_row = it->second;
            }
            ts->sprite_variants[id] = var;
        }
    }

    dbg( D_INFO ) << "Loaded " << ts->palettes.size() << " palettes and "
                  << ts->sprite_variants.size() << " sprite variants from " << path;
}

godot::Ref<godot::Image> godot_tileset::build_palette_image() const
{
    if( palettes.empty() ) {
        return {};
    }
    size_t width = 0;
    for( const godot_palette &pal : palettes ) {
        width = std::max( width, pal.ramp.size() );
    }
    // Row 0 is the identity row, so palette_row can stay 1-based on both sides
    // and a lookup that somehow arrives with 0 is a no-op rather than a
    // recolouring to whatever happened to be first.
    const size_t height = palettes.size() + 1;
    godot::PackedByteArray bytes;
    bytes.resize( static_cast<int64_t>( width * height * 4 ) );
    uint8_t *dst = reinterpret_cast<uint8_t *>( bytes.ptrw() );
    for( size_t x = 0; x < width; ++x ) {
        const uint8_t v = width > 1
                          ? static_cast<uint8_t>( x * 255 / ( width - 1 ) )
                          : uint8_t{ 255 };
        dst[x * 4 + 0] = v;
        dst[x * 4 + 1] = v;
        dst[x * 4 + 2] = v;
        dst[x * 4 + 3] = 255;
    }
    for( size_t row = 0; row < palettes.size(); ++row ) {
        const std::vector<std::array<uint8_t, 3>> &ramp = palettes[row].ramp;
        for( size_t x = 0; x < width; ++x ) {
            // Ramps shorter than the widest are stretched, not padded: a
            // three-colour palette should still span the full luminance range.
            const size_t src = ramp.size() == 1
                               ? 0
                               : x * ( ramp.size() - 1 ) / std::max<size_t>( 1, width - 1 );
            const std::array<uint8_t, 3> &c = ramp[std::min( src, ramp.size() - 1 )];
            const size_t i = ( ( row + 1 ) * width + x ) * 4;
            dst[i + 0] = c[0];
            dst[i + 1] = c[1];
            dst[i + 2] = c[2];
            dst[i + 3] = 255;
        }
    }
    return godot::Image::create_from_data( static_cast<int32_t>( width ),
                                           static_cast<int32_t>( height ), false,
                                           godot::Image::FORMAT_RGBA8, bytes );
}

void godot_tileset_loader::parse_atlases( const JsonObject &config,
        const cata_path &tileset_root,
        const cata_path &img_path, const bool pump_events )
{
    ( void )pump_events;
    if( config.has_array( "tiles-new" ) ) {
        // new system, several entries
        // When loading multiple tileset images this defines where
        // the tiles from the most recently loaded image start from.
        for( const JsonObject tile_part_def : config.get_array( "tiles-new" ) ) {
            const cata_path tileset_image_path = tileset_root / tile_part_def.get_string( "file" );
            R = -1;
            G = -1;
            B = -1;
            if( tile_part_def.has_object( "transparency" ) ) {
                JsonObject tra = tile_part_def.get_object( "transparency" );
                R = tra.get_int( "R" );
                G = tra.get_int( "G" );
                B = tra.get_int( "B" );
            }
            sprite_width = tile_part_def.get_int( "sprite_width", ts->tile_width_ );
            sprite_height = tile_part_def.get_int( "sprite_height", ts->tile_height_ );
            // Now load the tile definitions for the loaded tileset image.
            sprite_offset.x = tile_part_def.get_int( "sprite_offset_x", 0 );
            sprite_offset.y = tile_part_def.get_int( "sprite_offset_y", 0 );
            sprite_offset_retracted.x = tile_part_def.get_int( "sprite_offset_x_retracted",
                                        sprite_offset.x );
            sprite_offset_retracted.y = tile_part_def.get_int( "sprite_offset_y_retracted",
                                        sprite_offset.y );
            sprite_pixelscale = tile_part_def.get_float( "pixelscale", 1.0 );
            // Update maximum tile extent
            ts->max_tile_extent_ = half_open_rectangle<point> {
                {
                    std::min( { ts->max_tile_extent_.p_min.x, sprite_offset.x,
                                sprite_offset_retracted.x } ),
                    std::min( { ts->max_tile_extent_.p_min.y, sprite_offset.y,
                                sprite_offset_retracted.y } ),
                }, {
                    std::max( ts->max_tile_extent_.p_max.x,
                              sprite_width + std::max( sprite_offset.x, sprite_offset_retracted.x ) ),
                    std::max( ts->max_tile_extent_.p_max.y,
                              sprite_height + std::max( sprite_offset.y, sprite_offset_retracted.y ) ),
                }
            };
            // First read the tileset image header to count tiles.
            dbg( D_INFO ) << "Attempting to Load Tileset file " << tileset_image_path;
            read_image_dimensions( tileset_image_path, R, G, B );
            parse_mappings( tile_part_def );
            if( tile_part_def.has_member( "ascii" ) ) {
                load_ascii( tile_part_def );
            }
            // Make sure the tile definitions of the next tileset image don't
            // override the current ones.
            offset += size;
        }
    } else {
        sprite_width = ts->tile_width_;
        sprite_height = ts->tile_height_;
        sprite_offset = point::zero;
        sprite_offset_retracted = point::zero;
        sprite_pixelscale = 1.0;
        R = -1;
        G = -1;
        B = -1;
        // old system, no tile file path entry, only one array of tiles
        dbg( D_INFO ) << "Attempting to Load Tileset file " << img_path;
        read_image_dimensions( img_path, R, G, B );
        parse_mappings( config );
        offset += size;
    }

    // allows a tileset to override the order of mutation images being applied to a character
    if( config.has_array( "overlay_ordering" ) ) {
        load_overlay_ordering_into_array( config, tileset_mutation_overlay_ordering );
    }
}

void godot_tileset_loader::read_image_dimensions( const cata_path &img_path,
        const int kr, const int kg, const int kb )
{
    cata_assert( sprite_width > 0 );
    cata_assert( sprite_height > 0 );
    const godot::Ref<godot::Image> tile_atlas = load_atlas_image( img_path );
    tile_atlas_width = tile_atlas->get_width();

    const int expected_tilecount = ( tile_atlas->get_width() / sprite_width ) *
                                   ( tile_atlas->get_height() / sprite_height );

    atlas_descriptor desc;
    desc.image_path = img_path;
    desc.color_key_r = kr;
    desc.color_key_g = kg;
    desc.color_key_b = kb;
    desc.sprite_width = sprite_width;
    desc.sprite_height = sprite_height;
    desc.atlas_offset = offset;
    desc.expected_tilecount = expected_tilecount;
    // Keep the decoded image alive so upload_atlases does not re-decode it.
    desc.image = tile_atlas;
    atlases_.push_back( std::move( desc ) );

    size = expected_tilecount;
}

void godot_tileset_loader::load_layers( const JsonObject &config )
{
    for( const JsonObject item : config.get_array( "variants" ) ) {
        if( item.has_member( "context" ) && ( item.has_array( "item_variants" ) ||
                                              item.has_array( "field_variants" ) ) ) {

            std::string context;
            std::set<std::string> flags;
            std::string append_suffix;
            furn_str_id furn_exists;
            ter_str_id ter_exists;
            if( item.has_string( "context" ) ) {
                context = item.get_string( "context" );
                furn_exists = furn_str_id( context );
                ter_exists = ter_str_id( context );
                if( !furn_exists.is_valid() && !ter_exists.is_valid() ) {
                    debugmsg( "Layering data: %s not a valid furniture/terrain object", context );
                }
            }
            //currently, only one flag can be defined, and must be in an array
            else if( item.has_array( "context" ) ) {
                context = item.get_array( "context" ).next_value().get_string();
            }

            if( item.has_string( "append_variants" ) ) {
                append_suffix = item.get_string( "append_variants" );
                if( append_suffix.empty() ) {
                    config.throw_error( "append_variants cannot be empty string" );
                }
            }
            std::vector<godot_layer_context_sprites> item_layers;
            std::vector<godot_layer_context_sprites> field_layers;
            if( item.has_array( "item_variants" ) ) {
                for( const JsonObject vars : item.get_array( "item_variants" ) ) {
                    if( vars.has_member( "item" ) && vars.has_member( "layer" ) ) {
                        godot_layer_context_sprites lcs;
                        lcs.id = vars.get_string( "item" );

                        lcs.layer = vars.get_int( "layer" );
                        point offset;
                        if( vars.has_member( "offset_x" ) ) {
                            offset.x = vars.get_int( "offset_x" );
                        }
                        if( vars.has_member( "offset_y" ) ) {
                            offset.y = vars.get_int( "offset_y" );
                        }
                        lcs.offset = offset;
                        lcs.append_suffix = append_suffix;

                        int total_weight = 0;
                        if( vars.has_array( "sprite" ) ) {
                            for( const JsonObject sprites : vars.get_array( "sprite" ) ) {
                                std::string id = sprites.get_string( "id" );
                                int weight = sprites.get_int( "weight", 1 );
                                lcs.sprite.emplace( id, weight );
                                total_weight += weight;
                            }
                        } else {
                            //default if unprovided = item name
                            lcs.sprite.emplace( lcs.id, 1 );
                            total_weight = 1;
                        }
                        lcs.total_weight = total_weight;
                        item_layers.push_back( lcs );
                    } else {
                        config.throw_error( "items configured incorrectly" );
                    }
                }
                // sort them based on layering so we can draw them correctly
                std::sort( item_layers.begin(), item_layers.end(), []( const godot_layer_context_sprites & a,
                const godot_layer_context_sprites & b ) {
                    return a.layer < b.layer;
                } );
                ts->item_layer_data.emplace( context, item_layers );
            }
            if( item.has_array( "field_variants" ) ) {
                for( const JsonObject vars : item.get_array( "field_variants" ) ) {
                    if( vars.has_member( "field" ) && vars.has_array( "sprite" ) ) {
                        godot_layer_context_sprites lcs;
                        lcs.id = vars.get_string( "field" );
                        point offset;
                        if( vars.has_member( "offset_x" ) ) {
                            offset.x = vars.get_int( "offset_x" );
                        }
                        if( vars.has_member( "offset_y" ) ) {
                            offset.y = vars.get_int( "offset_y" );
                        }
                        lcs.offset = offset;

                        int total_weight = 0;
                        for( const JsonObject sprites : vars.get_array( "sprite" ) ) {
                            std::string id = sprites.get_string( "id" );
                            int weight = sprites.get_int( "weight", 1 );
                            lcs.sprite.emplace( id, weight );

                            total_weight += weight;
                        }
                        lcs.total_weight = total_weight;
                        field_layers.push_back( lcs );
                    } else {
                        config.throw_error( "fields configured incorrectly" );
                    }
                }
                ts->field_layer_data.emplace( context, field_layers );
            }
        } else {
            config.throw_error( "layering configured incorrectly" );
        }
    }
}

void godot_tileset_loader::process_variations_after_loading(
    weighted_int_list<std::vector<int>> &vs ) const
{
    // loop through all of the variations
    for( auto &v : vs ) {
        // in a given variation, erase any invalid sprite ids
        v.first.erase(
            std::remove_if(
                v.first.begin(),
                v.first.end(),
        [&]( int id ) {
            return id >= offset || id < 0;
        } ),
        v.first.end()
        );
    }
    // erase any variations with no valid sprite ids left
    vs.erase(
        std::remove_if(
            vs.begin(),
            vs.end(),
    [&]( const std::pair<std::vector<int>, int> &o ) {
        return o.first.empty();
    }
        ),
    vs.end()
    );
    // populate the bookkeeping table used for selecting sprite variations
    vs.precalc();
}

void godot_tileset_loader::add_ascii_subtile( godot_tile_type &curr_tile,
        const std::string &t_id, int sprite_id, const std::string &s_id )
{
    const std::string m_id = str_cat( t_id, "_", s_id );
    godot_tile_type curr_subtile;
    curr_subtile.fg.add( std::vector<int>( {sprite_id} ), 1 );
    curr_subtile.rotates = true;
    curr_tile.available_subtiles.push_back( s_id );
    ts->create_tile_type( m_id, std::move( curr_subtile ) );
}

void godot_tileset_loader::load_ascii( const JsonObject &config )
{
    if( !config.has_member( "ascii" ) ) {
        config.throw_error( "\"ascii\" section missing" );
    }
    for( const JsonObject entry : config.get_array( "ascii" ) ) {
        load_ascii_set( entry );
    }
}

void godot_tileset_loader::load_ascii_set( const JsonObject &entry )
{
    // tile for ASCII char 0 is at `in_image_offset`,
    // the other ASCII chars follow from there.
    const int in_image_offset = entry.get_int( "offset" );
    if( in_image_offset >= size ) {
        entry.throw_error_at( "offset", "invalid offset (out of range)" );
    }
    // color, of the ASCII char. Can be -1 to indicate all/default colors.
    int FG = -1;
    const std::string scolor = entry.get_string( "color", "DEFAULT" );
    if( scolor == "BLACK" ) {
        FG = catacurses::black;
    } else if( scolor == "RED" ) {
        FG = catacurses::red;
    } else if( scolor == "GREEN" ) {
        FG = catacurses::green;
    } else if( scolor == "YELLOW" ) {
        FG = catacurses::yellow;
    } else if( scolor == "BLUE" ) {
        FG = catacurses::blue;
    } else if( scolor == "MAGENTA" ) {
        FG = catacurses::magenta;
    } else if( scolor == "CYAN" ) {
        FG = catacurses::cyan;
    } else if( scolor == "WHITE" ) {
        FG = catacurses::white;
    } else if( scolor == "DEFAULT" ) {
        FG = -1;
    } else {
        entry.throw_error_at( "color", "invalid color for ASCII" );
    }
    // Add an offset for bold colors (ncurses has this bold attribute,
    // this mimics it). bold does not apply to default color.
    if( FG != -1 && entry.get_bool( "bold", false ) ) {
        FG += 8;
    }
    const int base_offset = offset + in_image_offset;
    // Finally load all 256 ASCII chars (actually extended ASCII)
    for( int ascii_char = 0; ascii_char < 256; ascii_char++ ) {
        const int index_in_image = ascii_char + in_image_offset;
        if( index_in_image < 0 || index_in_image >= size ) {
            // Out of range is ignored for now.
            continue;
        }
        const std::string id = get_ascii_tile_id( ascii_char, FG, -1 );
        godot_tile_type curr_tile;
        curr_tile.offset = sprite_offset;
        curr_tile.offset_retracted = sprite_offset_retracted;
        curr_tile.pixelscale = sprite_pixelscale;
        auto &sprites = *curr_tile.fg.add( std::vector<int>( {index_in_image + offset} ), 1 );
        switch( ascii_char ) {
            // box bottom/top side (horizontal line)
            case LINE_OXOX_C:
                sprites[0] = 196 + base_offset;
                break;
            // box left/right side (vertical line)
            case LINE_XOXO_C:
                sprites[0] = 179 + base_offset;
                break;
            // box top left
            case LINE_OXXO_C:
                sprites[0] = 218 + base_offset;
                break;
            // box top right
            case LINE_OOXX_C:
                sprites[0] = 191 + base_offset;
                break;
            // box bottom right
            case LINE_XOOX_C:
                sprites[0] = 217 + base_offset;
                break;
            // box bottom left
            case LINE_XXOO_C:
                sprites[0] = 192 + base_offset;
                break;
            // box bottom north T (left, right, up)
            case LINE_XXOX_C:
                sprites[0] = 193 + base_offset;
                break;
            // box bottom east T (up, right, down)
            case LINE_XXXO_C:
                sprites[0] = 195 + base_offset;
                break;
            // box bottom south T (left, right, down)
            case LINE_OXXX_C:
                sprites[0] = 194 + base_offset;
                break;
            // box X (left down up right)
            case LINE_XXXX_C:
                sprites[0] = 197 + base_offset;
                break;
            // box bottom east T (left, down, up)
            case LINE_XOXX_C:
                sprites[0] = 180 + base_offset;
                break;
        }
        if( ascii_char == LINE_XOXO_C || ascii_char == LINE_OXOX_C ) {
            curr_tile.rotates = false;
            curr_tile.multitile = true;
            add_ascii_subtile( curr_tile, id, 197 + base_offset, "center" );
            add_ascii_subtile( curr_tile, id, 218 + base_offset, "corner" );
            add_ascii_subtile( curr_tile, id, 179 + base_offset, "edge" );
            add_ascii_subtile( curr_tile, id, 194 + base_offset, "t_connection" );
            add_ascii_subtile( curr_tile, id, 179 + base_offset, "end_piece" );
            add_ascii_subtile( curr_tile, id, 197 + base_offset, "unconnected" );
        }
        ts->create_tile_type( id, std::move( curr_tile ) );
    }
}

void godot_tileset_loader::parse_mappings( const JsonObject &config )
{
    if( !config.has_member( "tiles" ) ) {
        config.throw_error( "\"tiles\" section missing" );
    }

    for( const JsonObject entry : config.get_array( "tiles" ) ) {
        std::vector<std::string> ids;
        if( entry.has_string( "id" ) ) {
            ids.push_back( entry.get_string( "id" ) );
        } else if( entry.has_array( "id" ) ) {
            ids = entry.get_string_array( "id" );
        }
        for( const std::string &t_id : ids ) {
            godot_tile_type &curr_tile = load_tile( entry, t_id );
            curr_tile.offset = sprite_offset;
            curr_tile.offset_retracted = sprite_offset_retracted;
            curr_tile.pixelscale = sprite_pixelscale;
            const bool t_multi = entry.get_bool( "multitile", false );
            const bool t_rota = entry.get_bool( "rotates", t_multi );
            const int t_h3d = entry.get_int( "height_3d", 0 );
            if( t_multi ) {
                // fetch additional tiles
                for( const JsonObject subentry : entry.get_array( "additional_tiles" ) ) {
                    const std::string s_id = subentry.get_string( "id" );
                    const std::string m_id = str_cat( t_id, "_", s_id );
                    godot_tile_type &curr_subtile = load_tile( subentry, m_id );
                    curr_subtile.offset = sprite_offset;
                    curr_subtile.offset_retracted = sprite_offset_retracted;
                    curr_subtile.pixelscale = sprite_pixelscale;
                    curr_subtile.rotates = true;
                    curr_subtile.height_3d = t_h3d;
                    curr_subtile.animated = subentry.get_bool( "animated", false );
                    curr_tile.available_subtiles.push_back( s_id );
                }
            } else if( entry.has_array( "additional_tiles" ) ) {
                try {
                    entry.throw_error( "Additional tiles defined, but 'multitile' is not true." );
                } catch( const JsonError &err ) {
                    debugmsg( "(json-error)\n%s", err.what() );
                }
            }
            // write the information of the base tile to curr_tile
            curr_tile.multitile = t_multi;
            curr_tile.rotates = t_rota;
            curr_tile.height_3d = t_h3d;
            curr_tile.animated = entry.get_bool( "animated", false );
        }
    }
    dbg( D_INFO ) << "Tile Width: " << ts->tile_width_ << " Tile Height: " << ts->tile_height_ <<
                  " Tile Definitions: " << ts->tile_ids.size();
}

/**
 * Load a tile definition and add it to the @ref godot_tileset::tile_ids map.
 * All loaded tiles go into one vector (@ref godot_tileset::tile_values), their
 * index in it is their id.
 * The JSON data (loaded here) contains tile ids relative to the associated image.
 * They are translated into global ids by adding the @p offset, which is the
 * number of previously loaded tiles (excluding the tiles from the associated
 * image).
 * @param id The id of the new tile definition (which is the key in
 * @ref godot_tileset::tile_ids). Any existing definition of the same id is
 * overridden.
 * @return A reference to the loaded tile inside the @ref godot_tileset::tile_ids map.
 */
godot_tile_type &godot_tileset_loader::load_tile( const JsonObject &entry,
        const std::string &id )
{
    if( ts->find_tile_type( id ) ) {
        ts->duplicate_ids.insert( id );
    }
    godot_tile_type curr_subtile;

    load_tile_spritelists( entry, curr_subtile.fg, "fg" );
    load_tile_spritelists( entry, curr_subtile.bg, "bg" );

    return ts->create_tile_type( id, std::move( curr_subtile ) );
}

void godot_tileset_loader::load_tile_spritelists( const JsonObject &entry,
        weighted_int_list<std::vector<int>> &vs,
        std::string_view objname ) const
{
    // json array indicates rotations or variations
    if( entry.has_array( objname ) ) {
        JsonArray g_array = entry.get_array( objname );
        // int elements of array indicates rotations
        // create one variation, populate sprite_ids with list of ints
        if( g_array.test_int() ) {
            std::vector<int> v;
            for( const int entry : g_array ) {
                const int sprite_id = entry + sprite_id_offset;
                if( sprite_id >= 0 ) {
                    v.push_back( sprite_id );
                }
            }
            vs.add( v, 1 );
        }
        // object elements of array indicates variations
        // create one variation per object
        else if( g_array.test_object() ) {
            for( const JsonObject vo : g_array ) {
                std::vector<int> v;
                int weight = vo.get_int( "weight" );
                // negative weight is invalid
                if( weight < 0 ) {
                    vo.throw_error_at( objname, "Invalid weight for sprite variation (<0)" );
                }
                // int sprite means one sprite
                if( vo.has_int( "sprite" ) ) {
                    const int sprite_id = vo.get_int( "sprite" ) + sprite_id_offset;
                    if( sprite_id >= 0 ) {
                        v.push_back( sprite_id );
                    }
                }
                // array sprite means rotations
                else if( vo.has_array( "sprite" ) ) {
                    for( const int entry : vo.get_array( "sprite" ) ) {
                        const int sprite_id = entry + sprite_id_offset;
                        if( sprite_id >= 0 ) {
                            v.push_back( sprite_id );
                        }
                    }
                }
                if( v.size() != 1 &&
                    v.size() != 2 &&
                    v.size() != 4 ) {
                    vo.throw_error_at( objname, "Invalid number of sprites (not 1, 2, or 4)" );
                }
                vs.add( v, weight );
            }
        }
    }
    // json int indicates a single sprite id
    else if( entry.has_int( objname ) && entry.get_int( objname ) >= 0 ) {
        vs.add( std::vector<int>( {entry.get_int( objname ) + sprite_id_offset} ), 1 );
    }
}

void godot_tileset_loader::upload_atlases()
{
    int total = 0;
    for( const atlas_descriptor &d : atlases_ ) {
        total += d.expected_tilecount;
    }
    const std::optional<int> highlight_idx = ts->get_default_item_highlight_index();
    const int highlight_extra = highlight_idx ? 1 : 0;

    // The synthetic highlight adds one slot past the atlas tilecount.
    ts->tile_values.assign( static_cast<size_t>( total + highlight_extra ), godot_texture() );

    /**
     * Only the unfiltered atlas is built.
     *
     * SDL keeps five more copies of every sprite -- grayscale, nightvision,
     * overexposed, memory and silhouette -- because SDL2 had no cheap way to shade
     * a sprite per-pixel, so it swapped the whole atlas per light level. The Godot
     * backend multiplies map_draw_cmd::tint in on the GPU instead (ADR-003), so
     * those five were built, uploaded, and never sampled: six times the atlas
     * memory for one atlas's worth of output.
     */
    using tiles_pixel_color_entry = std::tuple<std::vector<godot_texture>*, std::string>;
    const std::array<tiles_pixel_color_entry, 1> tile_values_data = {{
            { std::make_tuple( &ts->tile_values, "color_pixel_none" ) }
        }
    };

    for( const atlas_descriptor &d : atlases_ ) {
        cata_assert( d.sprite_width > 0 );
        cata_assert( d.sprite_height > 0 );
        cata_assert( d.image.is_valid() );

        // Decode the atlas once, applying the transparency color key. The
        // color-filter variants preserve the alpha channel, so per-sprite
        // opaque bounds are computed once from this unfiltered data.
        const std::vector<uint8_t> base = image_to_rgba( d.image, d.color_key_r,
                                          d.color_key_g, d.color_key_b );
        const int cols = d.image->get_width() / d.sprite_width;

        const rect_range<pixel_rect> input_range( d.sprite_width, d.sprite_height,
                point( d.image->get_width() / d.sprite_width,
                       d.image->get_height() / d.sprite_height ) );
        // Pre-size to accommodate the maximum index this chunk can produce.
        const size_t max_index = static_cast<size_t>( d.atlas_offset ) +
                                 static_cast<size_t>( cols ) * ( d.image->get_height() / d.sprite_height );
        std::vector<pixel_rect> opaque_bounds( max_index + cols, pixel_rect{ 0, 0, 0, 0 } );
        for( const pixel_rect rect : input_range ) {
            const size_t index = static_cast<size_t>( d.atlas_offset ) +
                                 ( rect.x / d.sprite_width ) + ( rect.y / d.sprite_height ) * cols;
            if( index < opaque_bounds.size() ) {
                opaque_bounds[index] = compute_opaque_rect( base, d.image->get_width(), rect );
            }
        }

        for( const tiles_pixel_color_entry &entry : tile_values_data ) {
            const pixel_filter filter = get_color_pixel_function( std::get<1>( entry ) );
            const godot::Ref<godot::ImageTexture> atlas_texture =
                filter ? make_atlas_texture( d.image, apply_color_filter( base, filter ) )
                : make_atlas_texture( d.image, base );
            cata_assert( atlas_texture.is_valid() );
            // Keep the atlas texture alive for the lifetime of the tileset;
            // the sprites below only reference it through a source rect.
            ts->atlas_textures.push_back( atlas_texture );

            // Emit the per-sprite textures into the variant target.
            std::vector<godot_texture> &target = *std::get<0>( entry );
            for( const pixel_rect rect : input_range ) {
                const size_t index = static_cast<size_t>( d.atlas_offset ) +
                                     ( rect.x / d.sprite_width ) + ( rect.y / d.sprite_height ) * cols;
                cata_assert( index < target.size() );
                cata_assert( !target[index].is_valid() );
                const pixel_rect &opaque = index < opaque_bounds.size()
                                           ? opaque_bounds[index]
                                           : pixel_rect{ 0, 0, rect.w, rect.h };
                target[index] = godot_texture( atlas_texture, rect.x, rect.y, rect.w, rect.h,
                                               opaque.x, opaque.y, opaque.w, opaque.h );
            }
        }
    }

    // Fill the reserved synthetic highlight slot if the tileset lacks its own.
    if( highlight_idx ) {
        const int idx = *highlight_idx;
        cata_assert( idx >= 0 && idx < static_cast<int>( ts->tile_values.size() ) );
        std::vector<uint8_t> highlight_rgba( static_cast<size_t>( ts->tile_width_ ) *
                ts->tile_height_ * 4, 0 );
        for( size_t i = 0; i < highlight_rgba.size(); i += 4 ) {
            // 64-bit highlight: RGB(0, 0, 127) with 50% alpha, matching the
            // SDL loader's FillRect(MapRGBA(0, 0, 127, 127)).
            highlight_rgba[i + 2] = 127;
            highlight_rgba[i + 3] = 127;
        }
        godot::PackedByteArray bytes;
        bytes.resize( static_cast<int64_t>( highlight_rgba.size() ) );
        std::memcpy( bytes.ptrw(), highlight_rgba.data(), highlight_rgba.size() );
        const godot::Ref<godot::Image> highlight_image = godot::Image::create_from_data(
                    ts->tile_width_, ts->tile_height_, false,
                    godot::Image::FORMAT_RGBA8, bytes );
        const godot::Ref<godot::ImageTexture> highlight_texture =
            godot::ImageTexture::create_from_image( highlight_image );
        ts->atlas_textures.push_back( highlight_texture );
        ts->tile_values[idx] = godot_texture( highlight_texture, 0, 0, ts->tile_width_,
                                              ts->tile_height_ );
    }
}

} // namespace godot_backend

#endif // GODOT
