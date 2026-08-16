#include "godot_map_snapshot.h"
#include "godot_overmap_snapshot.h"

#if defined(GODOT)

#include "avatar.h"
#include "catacharset.h"
#include "character.h"
#include "color.h"
#include "creature.h"
#include "cursesport.h"
#include "field.h"
#include "field_type.h"
#include "game.h"
#include "enums.h"
#include "godot_light_snapshot.h"
#include "godot_tileset_loader.h"
#include "item.h"
#include "itype.h"
#include "lightmap.h"
#include "tile_connections.h"
#include "map.h"
#include "map_memory.h"
#include "mapdata.h"
#include "monster.h"
#include "mtype.h"
#include "cata_utility.h"
#include "options.h"
#include "output.h"
#include "trap.h"
#include "type_id.h"
#include "veh_type.h"
#include "vehicle.h"
#include "vpart_position.h"
#include "weather.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>

#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot_backend
{

const char *category_id( const sprite_category cat )
{
    switch( cat ) {
        case sprite_category::terrain: return "terrain";
        case sprite_category::furniture: return "furniture";
        case sprite_category::trap: return "trap";
        case sprite_category::field: return "field";
        case sprite_category::vehicle_part: return "vehicle_part";
        case sprite_category::item: return "item";
        case sprite_category::monster: return "monster";
        // No "unknown_character" convention exists; the avatar falls through to
        // the generic placeholder.
        case sprite_category::character:
        case sprite_category::none:
        case sprite_category::last:
            break;
    }
    return nullptr;
}

namespace
{

/// Human-readable @ref sprite_fallback, for the coverage report and the debug
/// overlay. Kept beside category_id so the two vocabularies stay together.
const char *fallback_name( const sprite_fallback level )
{
    switch( level ) {
        case sprite_fallback::exact: return "exact";
        case sprite_fallback::looks_like: return "looks_like";
        case sprite_fallback::ascii: return "ascii";
        case sprite_fallback::category: return "category";
        case sprite_fallback::glyph: return "glyph";
        case sprite_fallback::missing: return "missing";
        case sprite_fallback::last: break;
    }
    return "?";
}

MapSnapshot g_map_snapshot;
godot_tileset g_tileset;
// Texture pointer → exported atlas index (filled at load).
std::unordered_map<const void *, int> g_atlas_ptr_index;

/// Look @p id up without following looks_like or any fallback.
///
/// Needed to test whether an optional id (a multitile subtile, a field
/// intensity variant) exists at all, which @ref resolve_sprite cannot answer --
/// it always succeeds at some level.
bool find_tile_by_id_exact( const std::string &id )
{
    return g_tileset.find_tile_type( id ) != nullptr;
}

/// The vehicle part behind a tileset id, which arrives as "vp_<part>" or
/// "vp_<part>_<variant>". Trailing variant segments are dropped one at a time
/// until something valid is left, because a variant name may itself contain
/// underscores.
vpart_id vpart_from_tileset_id( const std::string &id )
{
    std::string base = id.compare( 0, 3, "vp_" ) == 0 ? id.substr( 3 ) : id;
    while( !base.empty() ) {
        const vpart_id candidate( base );
        if( candidate.is_valid() ) {
            return candidate;
        }
        const size_t cut = base.rfind( '_' );
        if( cut == std::string::npos || cut == 0 ) {
            break;
        }
        base.erase( cut );
    }
    return vpart_id::NULL_ID();
}

/// The `looks_like` this id declares in JSON, or "" when it declares none.
///
/// Every category that has the field is covered. The chain used to run for
/// terrain, furniture and monsters only, so an item or vehicle part variant
/// with no art of its own skipped straight to "unknown" even when the JSON said
/// exactly what it should borrow.
std::string looks_like_of( const std::string &id, const sprite_category cat )
{
    switch( cat ) {
        case sprite_category::terrain: {
            const ter_str_id sid( id );
            return sid.is_valid() ? sid.obj().looks_like : std::string();
        }
        case sprite_category::furniture: {
            const furn_str_id sid( id );
            return sid.is_valid() ? sid.obj().looks_like : std::string();
        }
        case sprite_category::monster: {
            const mtype_id sid( id );
            return sid.is_valid() ? sid.obj().looks_like : std::string();
        }
        case sprite_category::item: {
            const itype_id sid( id );
            return sid.is_valid() ? sid.obj().looks_like.str() : std::string();
        }
        case sprite_category::field: {
            const field_type_str_id sid( id );
            return sid.is_valid() ? sid.obj().looks_like : std::string();
        }
        case sprite_category::vehicle_part: {
            const vpart_id vpid = vpart_from_tileset_id( id );
            if( vpid.is_null() || !vpid.is_valid() ) {
                return {};
            }
            const std::string &ll = vpid.obj().looks_like;
            // Back into tileset-id space: the sprite ids are "vp_"-prefixed.
            return ll.empty() ? std::string() : "vp_" + ll;
        }
        // Traps and the avatar have no looks_like in JSON.
        case sprite_category::trap:
        case sprite_category::character:
        case sprite_category::none:
        case sprite_category::last:
            break;
    }
    return {};
}

/// The symbol and colour a type declares in JSON, for the two fallbacks that
/// draw a character rather than a sprite. codepoint 0 means "this build does
/// not know this id", which is the one case nothing can be drawn for.
struct json_glyph {
    uint32_t codepoint = 0;
    nc_color color = c_white;
};

json_glyph glyph_for( const std::string &id, const sprite_category cat )
{
    switch( cat ) {
        case sprite_category::terrain: {
            const ter_str_id sid( id );
            if( sid.is_valid() ) {
                return { static_cast<uint32_t>( sid.obj().symbol() ), sid.obj().color() };
            }
            break;
        }
        case sprite_category::furniture: {
            const furn_str_id sid( id );
            if( sid.is_valid() ) {
                return { static_cast<uint32_t>( sid.obj().symbol() ), sid.obj().color() };
            }
            break;
        }
        case sprite_category::monster: {
            const mtype_id sid( id );
            if( sid.is_valid() ) {
                return { UTF8_getch( sid.obj().sym ), sid.obj().color };
            }
            break;
        }
        case sprite_category::item: {
            const itype_id sid( id );
            if( sid.is_valid() ) {
                const itype &ity = sid.obj();
                return { ity.sym.empty() ? static_cast<uint32_t>( ' ' ) : UTF8_getch( ity.sym ),
                         ity.color };
            }
            break;
        }
        case sprite_category::trap: {
            const trap_str_id sid( id );
            if( sid.is_valid() ) {
                return { static_cast<uint32_t>( sid.obj().sym ), sid.obj().color };
            }
            break;
        }
        case sprite_category::field: {
            const field_type_str_id sid( id );
            if( sid.is_valid() ) {
                const field_intensity_level &lvl = sid.obj().get_intensity_level();
                return { lvl.symbol, lvl.color };
            }
            break;
        }
        case sprite_category::vehicle_part: {
            const vpart_id vpid = vpart_from_tileset_id( id );
            if( !vpid.is_null() && vpid.is_valid() ) {
                const vpart_info &vpi = vpid.obj();
                uint32_t sym = static_cast<uint32_t>( '#' );
                if( !vpi.variants.empty() ) {
                    sym = static_cast<uint32_t>(
                              vpi.variants.begin()->second.get_symbol( 0_degrees, false ) );
                }
                return { sym, vpi.color };
            }
            break;
        }
        case sprite_category::character:
            return { static_cast<uint32_t>( '@' ), c_white };
        case sprite_category::none:
        case sprite_category::last:
            break;
    }
    return {};
}

/**
 * Resolve one entry of Character::get_overlay_ids to a tileset id.
 *
 * A faithful port of cata_tiles::find_overlay_looks_like, because the id shapes
 * are a convention shared with every existing tileset and inventing a second
 * spelling of them would mean no tileset's art fit. The convention:
 *
 *   overlay_[male_|female_][worn_|wielded_]<id>[_var_<variant>]
 *
 * and each step falls back -- gendered to ungendered, variant to base, an
 * active mutation to its inactive sprite, an item to whatever its JSON says it
 * looks like. Returns false when nothing in the chain exists, which is the
 * common case: Ultica draws about 3,400 overlays and the game has far more
 * items than that.
 *
 * @param out set only on success.
 */
bool resolve_overlay_id( const bool male, const std::string &overlay,
                         const std::string &variant, std::string &out )
{
    std::string looks_like = overlay;
    std::string over_type;
    if( overlay.compare( 0, 5, "worn_" ) == 0 ) {
        looks_like = overlay.substr( 5 );
        over_type = "worn_";
    } else if( overlay.compare( 0, 8, "wielded_" ) == 0 ) {
        looks_like = overlay.substr( 8 );
        over_type = "wielded_";
    }
    const char *gender = male ? "overlay_male_" : "overlay_female_";

    // Variants first, gendered before neutral. Twice around, because an active
    // mutation's variant should fall back to the inactive mutation's variant
    // before giving up on variants entirely.
    if( !variant.empty() ) {
        for( int i = 0; i < 2; ++i ) {
            const std::string suffix = over_type + looks_like + "_var_" + variant;
            if( find_tile_by_id_exact( gender + suffix ) ) {
                out = gender + suffix;
                return true;
            }
            if( find_tile_by_id_exact( "overlay_" + suffix ) ) {
                out = "overlay_" + suffix;
                return true;
            }
            if( looks_like.compare( 0, 16, "mutation_active_" ) == 0 ) {
                looks_like = "mutation_" + looks_like.substr( 16 );
                continue;
            }
            break;
        }
    }

    // Then the base sprite, following item looks_like as far as it goes. The
    // bound matches cata_tiles: modded JSON can point looks_like in a circle.
    for( int i = 0; i < 10 && !looks_like.empty(); ++i ) {
        if( find_tile_by_id_exact( gender + over_type + looks_like ) ) {
            out = gender + over_type + looks_like;
            return true;
        }
        if( find_tile_by_id_exact( "overlay_" + over_type + looks_like ) ) {
            out = "overlay_" + over_type + looks_like;
            return true;
        }
        if( looks_like.compare( 0, 16, "mutation_active_" ) == 0 ) {
            looks_like = "mutation_" + looks_like.substr( 16 );
            continue;
        }
        const itype_id as_item( looks_like );
        if( !as_item.is_valid() ) {
            break;
        }
        looks_like = as_item.obj().looks_like.str();
    }
    return false;
}

/// Collapse the several encodings of a box-drawing character onto the single
/// value the ASCII tile ids are keyed on. Same switch as cata_tiles: a wall's
/// symbol may arrive as an ncurses ACS code or as a Unicode box character, and
/// only the _C form has a sprite.
uint32_t normalize_line_symbol( const uint32_t sym )
{
    switch( sym ) {
        case LINE_XOXO: case LINE_XOXO_UNICODE: return LINE_XOXO_C;
        case LINE_OXOX: case LINE_OXOX_UNICODE: return LINE_OXOX_C;
        case LINE_XXOO: case LINE_XXOO_UNICODE: return LINE_XXOO_C;
        case LINE_OXXO: case LINE_OXXO_UNICODE: return LINE_OXXO_C;
        case LINE_OOXX: case LINE_OOXX_UNICODE: return LINE_OOXX_C;
        case LINE_XOOX: case LINE_XOOX_UNICODE: return LINE_XOOX_C;
        case LINE_XXXO: case LINE_XXXO_UNICODE: return LINE_XXXO_C;
        case LINE_XXOX: case LINE_XXOX_UNICODE: return LINE_XXOX_C;
        case LINE_XOXX: case LINE_XOXX_UNICODE: return LINE_XOXX_C;
        case LINE_OXXX: case LINE_OXXX_UNICODE: return LINE_OXXX_C;
        case LINE_XXXX: case LINE_XXXX_UNICODE: return LINE_XXXX_C;
        default: return sym;
    }
}

/// The tileset id an ASCII sprite is registered under, matching the loader's
/// own get_ascii_tile_id (src/godot_tileset_loader.cpp).
std::string ascii_tile_id( const uint32_t sym, const int fg )
{
    return std::string( { 'A', 'S', 'C', 'I', 'I', '_', static_cast<char>( sym ),
                          static_cast<char>( fg ), static_cast<char>( -1 )
                        } );
}

/// The ncurses foreground index a colour maps to, which is how ASCII sprites
/// are keyed. -1 when the colour is not one of the sixteen curses colours.
int curses_fg_index( const nc_color &col )
{
    const int pair_number = col.to_color_pair_index();
    if( pair_number < 0 ||
        pair_number >= static_cast<int>( cata_cursesport::colorpairs.size() ) ) {
        return -1;
    }
    return cata_cursesport::colorpairs[pair_number].FG + ( col.is_bold() ? 8 : 0 );
}

/// Pack an RGBA colour the way map_glyph_cmd::fg expects.
int32_t pack_rgba( const color &c )
{
    return static_cast<int32_t>( ( static_cast<uint32_t>( c.r ) << 24 ) |
                                 ( static_cast<uint32_t>( c.g ) << 16 ) |
                                 ( static_cast<uint32_t>( c.b ) << 8 ) |
                                 static_cast<uint32_t>( c.a ) );
}

/// What @ref resolve_sprite found, and how far it had to go to find it.
struct resolved_sprite {
    const godot_tile_type *tile = nullptr;
    sprite_fallback level = sprite_fallback::missing;
    /// Set when level is @ref sprite_fallback::glyph: draw this instead.
    uint32_t codepoint = 0;
    int32_t glyph_fg = 0;
    /// 1-based palette row for the shader to recolour through, 0 for none
    /// (SP-8).
    int32_t palette_row = 0;
};

/// Whether this terrain or furniture should bend in the wind (SP-7).
///
/// Flags first, because they are data the game already maintains and a mod's
/// new tree gets the behaviour for free. The id list is for the things CDDA has
/// no flag for -- as far as the game is concerned grass is terrain like a floor
/// is -- which is what makes this "opt-in per tile id" rather than automatic.
bool is_vegetation( const std::string &id, const map_data_common_t &data )
{
    static constexpr std::array<ter_furn_flag, 5> flags = {
        ter_furn_flag::TFLAG_TREE, ter_furn_flag::TFLAG_YOUNG,
        ter_furn_flag::TFLAG_SHRUB, ter_furn_flag::TFLAG_FLOWER,
        ter_furn_flag::TFLAG_PLANT,
    };
    for( const ter_furn_flag f : flags ) {
        if( data.has_flag( f ) ) {
            return true;
        }
    }
    // Checked against data/json/furniture_and_terrain: between them these cover
    // the 41 vegetation ids that carry none of the flags above -- grass most of
    // all, which is the one the player is looking at nearly all the time.
    static constexpr std::array<std::string_view, 5> prefixes = {
        "t_grass", "t_underbrush", "t_shrub", "t_moss", "f_flower",
    };
    for( const std::string_view prefix : prefixes ) {
        if( id.compare( 0, prefix.size(), prefix ) == 0 ) {
            return true;
        }
    }
    return false;
}

/**
 * Whether a sprite is drawn larger than the tile cell it belongs to.
 *
 * This is the line between a thing standing on a tile and a tile itself, and
 * it decides whether the sway shader may shear it. A sprite that exactly fills
 * its cell has to meet its neighbours pixel for pixel; moving its top edge
 * opens a seam on one side and overlaps on the other, which is what tore a
 * field of grass into disconnected squares. A sprite that overhangs its cell
 * was drawn as an object by someone who knew it would not tile, and has
 * transparent margins to move into.
 *
 * The tileset states which is which, so this asks rather than guesses:
 * UltimateCataclysm draws t_grass at 32x32 on normal.png and t_grass_tall at
 * 32x64 on tall.png. Same plant, different kind of thing.
 */
bool sprite_overhangs_cell( const int sprite_idx, const int tile_w, const int tile_h )
{
    if( sprite_idx < 0 || sprite_idx >= static_cast<int>( g_tileset.tile_values.size() ) ) {
        return false;
    }
    const godot_texture &tex = g_tileset.tile_values[static_cast<size_t>( sprite_idx )];
    if( !tex.is_valid() ) {
        return false;
    }
    return tex.src_w() > tile_w || tex.src_h() > tile_h;
}

/**
 * Resolve a tileset id to something drawable (SP-1).
 *
 * Five steps, in the order cata_tiles::draw_from_id_string uses so a miss looks
 * the same here as it does under SDL:
 *
 *   1. the id itself;
 *   2. whatever its JSON `looks_like` points at, followed to the end;
 *   3. the tileset's own ASCII sprite for the type's symbol, first in its
 *      colour and then in the default colour;
 *   4. the "unknown_<category>" placeholder, then plain "unknown";
 *   5. the symbol itself, drawn as a font glyph.
 *
 * Only the last is new. SDL stops at step 4 and draws a question mark; a glyph
 * says what the tile is, which matters most for exactly the tilesets that are
 * missing the most art.
 */
resolved_sprite resolve_sprite( const std::string &id, const sprite_category cat )
{
    resolved_sprite out;
    if( id.empty() ) {
        return out;
    }

    // Step 0: a declared palette variant (SP-8). This is not a fallback -- it
    // is an author saying "draw that sprite, in these colours" -- so it happens
    // before the chain and does not count as a miss.
    std::string lookup = id;
    if( !g_tileset.sprite_variants.empty() ) {
        const auto var = g_tileset.sprite_variants.find( id );
        if( var != g_tileset.sprite_variants.end() ) {
            out.palette_row = var->second.palette_row;
            if( !var->second.sprite.empty() ) {
                lookup = var->second.sprite;
            }
        }
    }

    if( const godot_tile_type *t = g_tileset.find_tile_type( lookup ) ) {
        out.tile = t;
        out.level = sprite_fallback::exact;
        return out;
    }

    // Step 2. Bounded, because looks_like can be circular in modded JSON.
    std::string found = lookup;
    for( int jumps = 0; jumps < 8; ++jumps ) {
        const std::string next = looks_like_of( found, cat );
        if( next.empty() || next == found ) {
            break;
        }
        found = next;
        if( const godot_tile_type *t = g_tileset.find_tile_type( found ) ) {
            out.tile = t;
            out.level = sprite_fallback::looks_like;
            return out;
        }
    }

    // The symbol comes from the far end of the looks_like chain, as it does in
    // cata_tiles: a variant that borrows another type's art should borrow its
    // glyph too when neither has a sprite.
    const json_glyph jg = glyph_for( found, cat );
    const uint32_t sym = normalize_line_symbol( jg.codepoint );

    // Step 3. Only the low 256 code points have ASCII sprites.
    if( sym != 0 && sym < 256 ) {
        const int fg = curses_fg_index( jg.color );
        if( fg >= 0 ) {
            if( const godot_tile_type *t = g_tileset.find_tile_type( ascii_tile_id( sym, fg ) ) ) {
                out.tile = t;
                out.level = sprite_fallback::ascii;
                return out;
            }
        }
        if( const godot_tile_type *t = g_tileset.find_tile_type( ascii_tile_id( sym, -1 ) ) ) {
            out.tile = t;
            out.level = sprite_fallback::ascii;
            return out;
        }
    }

    // Step 4.
    if( const char *cat_id = category_id( cat ) ) {
        if( const godot_tile_type *t =
                g_tileset.find_tile_type( std::string( "unknown_" ) + cat_id ) ) {
            out.tile = t;
            out.level = sprite_fallback::category;
            return out;
        }
    }

    // Step 5. "unknown" is a question mark; the type's own symbol says more, so
    // it wins whenever the JSON supplies one.
    if( sym != 0 && sym != static_cast<uint32_t>( ' ' ) ) {
        out.level = sprite_fallback::glyph;
        out.codepoint = sym;
        out.glyph_fg = pack_rgba( curses_color_to_color( jg.color ) );
        return out;
    }
    if( const godot_tile_type *t = g_tileset.find_tile_type( "unknown" ) ) {
        out.tile = t;
        out.level = sprite_fallback::category;
        return out;
    }
    return out;
}

/// Sprite-variation seed for a stationary map tile.
///
/// Mirrors the `simple_point_hash` lambda in cata_tiles.cpp, which is file-local
/// there. It must stay identical (and keyed on *absolute* coordinates) or the
/// Godot renderer picks different variants than SDL for the same tile.
constexpr unsigned int simple_point_hash( const point &p )
{
    return static_cast<unsigned int>( p.x + p.y * 65536 );
}

/**
 * Map a neighbour connection mask (and rotate-to mask) to a multitile subtile
 * and rotation.
 *
 * Delegates to tile_connections::get_rotation_and_subtile rather than keeping a copy.
 * The copy that used to live here handled only the no-rotation case, so terrain
 * with a `rotates_to` group -- roads being the visible one -- always drew the
 * base 0-3 rotation. Ultica supplies eight and sixteen sprite variants for those,
 * indexed by the full 0-15 rotation, so the base value picked the wrong variant
 * and the road edges came out as mismatched wedges.
 */
void connections_to_subtile( const uint8_t val, const char rot_to, int &subtile, int &rotation )
{
    tile_connections::get_rotation_and_subtile( static_cast<char>( val ), rot_to, rotation, subtile );
}

/// Tileset suffix for a multitile subtile; subtiles are registered as
/// "<id>_<name>" by the loader.
const char *subtile_name( const int subtile )
{
    switch( static_cast<MULTITILE_TYPE>( subtile ) ) {
        case center: return "center";
        case corner: return "corner";
        case edge: return "edge";
        case t_connection: return "t_connection";
        case end_piece: return "end_piece";
        case unconnected: return "unconnected";
        default: return nullptr;
    }
}

/// Pack an 8-bit RGBA modulation colour the way map_draw_cmd::tint expects.
constexpr int32_t pack_tint( int r, int g, int b, int a )
{
    return static_cast<int32_t>( ( static_cast<uint32_t>( r & 0xFF ) << 24 ) |
                                 ( static_cast<uint32_t>( g & 0xFF ) << 16 ) |
                                 ( static_cast<uint32_t>( b & 0xFF ) << 8 ) |
                                 static_cast<uint32_t>( a & 0xFF ) );
}

/// Modulation for a tile at light level @p ll.
///
/// SDL picked one of five pre-tinted atlases here. Multiplying a colour in gives
/// the same families of look (dim, blue-shifted night, blown-out highlight)
/// without five extra copies of every sprite, and lets the values be continuous.
///
/// @param light_pass the Godot side is running the light texture (SP-3/SP-4),
///        which owns brightness from then on. All this may still carry is hue:
///        the green cast of night-vision goggles, which is a property of how the
///        player sees rather than of how much light there is. Returning the dim
///        colours as well would darken every tile twice.
int32_t tint_for_light( const lit_level ll, const bool nv_goggles, const bool light_pass )
{
    if( light_pass ) {
        return nv_goggles && ll == lit_level::LOW ? pack_tint( 190, 255, 190, 255 )
               : pack_tint( 255, 255, 255, 255 );
    }
    switch( ll ) {
        case lit_level::BRIGHT:
            // Slightly hot, so light sources read as sources.
            return pack_tint( 255, 250, 235, 255 );
        case lit_level::LIT:
            return pack_tint( 255, 255, 255, 255 );
        case lit_level::BRIGHT_ONLY:
            // Bright but indistinct: washed out rather than dark.
            return pack_tint( 230, 230, 225, 255 );
        case lit_level::LOW:
            // Hard to see. Night vision shifts the residual light green instead
            // of just darkening it.
            return nv_goggles ? pack_tint( 120, 190, 120, 255 )
                   : pack_tint( 105, 110, 135, 255 );
        case lit_level::MEMORIZED:
            // Remembered, not seen: dim and desaturated toward blue-grey.
            return pack_tint( 70, 74, 90, 255 );
        case lit_level::DARK:
        case lit_level::BLANK:
        default:
            return pack_tint( 45, 48, 62, 255 );
    }
}

/// The two modulations one tile needs.
///
/// `sprite` goes through the tile shader, which may be running the light pass.
/// `glyph` is painted with a font by glyph_layer.gd, which is not, so it always
/// carries the full CPU lighting. The two are identical while the pass is off.
struct light_tints {
    int32_t sprite = static_cast<int32_t>( 0xFFFFFFFF );
    int32_t glyph = static_cast<int32_t>( 0xFFFFFFFF );
};

light_tints tints_for_light( const lit_level ll, const bool nv_goggles, const bool light_pass )
{
    return { tint_for_light( ll, nv_goggles, light_pass ),
             tint_for_light( ll, nv_goggles, false ) };
}

struct picked_sprite {
    int index = -1;
    bool rotate = false;
};

/**
 * Choose a sprite, and decide whether it may be rotated at draw time.
 *
 * This mirrors the dispatch at the top of cata_tiles::draw_sprite_at, and getting
 * it wrong is visible: a weighted entry holds a *list*, and when that list has more
 * than one entry the tileset has drawn the four orientations itself. Those must be
 * indexed by the rotation, not rotated -- rotating an already-rotated sprite, and
 * picking which one by the variation seed, produces exactly the sort of scrambled
 * walls it did before this was fixed.
 *
 * @param is_fg foreground layer. A single background sprite is never rotated,
 *              matching SDL's `!rota_fg && size == 1` case.
 */
picked_sprite pick_sprite_rota( const weighted_int_list<std::vector<int>> &list,
                                const unsigned int seed, const bool is_fg, const int rota )
{
    const std::vector<int> *picked = list.pick( seed );
    if( !picked || picked->empty() ) {
        return {};
    }
    const std::vector<int> &sprites = *picked;
    if( sprites.size() == 1 ) {
        return { sprites[0], is_fg };
    }
    return { sprites[static_cast<size_t>( std::max( 0, rota ) ) % sprites.size()], false };
}

} // namespace

MapSnapshot &get_map_snapshot()
{
    return g_map_snapshot;
}

bool ensure_tileset_loaded( const std::string &tileset_id )
{
    return get_map_snapshot().ensure_tileset_loaded( tileset_id );
}

void release_godot_resources()
{
    // Both tilesets keep Ref<ImageTexture> alive; so does anything cached from
    // them. Dropping them here, rather than at static destruction, is the whole
    // point -- see the declaration in godot_backend.h.
    g_tileset.clear();
    get_overmap_snapshot().release_resources();
    get_map_snapshot().release_resources();
}

void update_map_snapshot()
{
    get_map_snapshot().update_from_game();
}

bool MapSnapshot::ensure_tileset_loaded( const std::string &tileset_id )
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        if( ready_ && tileset_id_ == tileset_id ) {
            return true;
        }
    }

    std::string id = tileset_id;
    if( TILESETS.find( id ) == TILESETS.end() ) {
        if( TILESETS.count( "UltimateCataclysm" ) ) {
            id = "UltimateCataclysm";
        } else if( !TILESETS.empty() ) {
            id = TILESETS.begin()->first;
        } else {
            godot::UtilityFunctions::printerr( "MapSnapshot: no tilesets found under gfx/" );
            return false;
        }
    }

    try {
        godot_tileset_loader loader;
        loader.load( g_tileset, id, /*precheck=*/false, /*pump_events=*/false,
                     /*terrain=*/false );
    } catch( const std::exception &e ) {
        godot::UtilityFunctions::printerr( "MapSnapshot: tileset load failed: ", e.what() );
        std::lock_guard<std::mutex> lock( mutex_ );
        ready_ = false;
        return false;
    }

    std::vector<atlas_pixels> atlases;
    g_atlas_ptr_index.clear();
    for( const godot_texture &sprite : g_tileset.tile_values ) {
        if( !sprite.is_valid() ) {
            continue;
        }
        const godot::Ref<godot::ImageTexture> &tex = sprite.get_texture();
        const void *key = tex.ptr();
        if( g_atlas_ptr_index.count( key ) ) {
            continue;
        }
        godot::Ref<godot::Image> img = tex->get_image();
        if( img.is_null() ) {
            continue;
        }
        if( img->get_format() != godot::Image::FORMAT_RGBA8 ) {
            img = img->duplicate();
            img->convert( godot::Image::FORMAT_RGBA8 );
        }
        atlas_pixels ap;
        ap.w = img->get_width();
        ap.h = img->get_height();
        const godot::PackedByteArray data = img->get_data();
        ap.rgba.assign( data.ptr(), data.ptr() + data.size() );
        g_atlas_ptr_index[key] = static_cast<int>( atlases.size() );
        atlases.push_back( std::move( ap ) );
    }

    {
        std::lock_guard<std::mutex> lock( mutex_ );
        tileset_id_ = id;
        tile_w_ = std::max( 1, g_tileset.get_tile_width() );
        tile_h_ = std::max( 1, g_tileset.get_tile_height() );
        atlases_ = std::move( atlases );
        ready_ = !atlases_.empty();
        // Converted to raw pixels immediately: the Ref must not outlive this
        // scope. See MapSnapshot::palette_pixels_.
        palette_pixels_ = {};
        if( const godot::Ref<godot::Image> pal = g_tileset.build_palette_image();
            pal.is_valid() ) {
            palette_pixels_.w = pal->get_width();
            palette_pixels_.h = pal->get_height();
            const godot::PackedByteArray data = pal->get_data();
            palette_pixels_.rgba.assign( data.ptr(), data.ptr() + data.size() );
        }
        cmds_.clear();
        glyphs_.clear();
        fields_.clear();
        avatar_overlays_.clear();
        // The coverage report is per tileset: switching tilesets changes which
        // ids are missing, so carrying the old counts over would be a lie.
        coverage_.clear();
        fallback_counts_ = {};
        layer_counts_ = {};
    }

    godot::UtilityFunctions::print(
        "MapSnapshot: loaded tileset ", id.c_str(),
        " atlases=", static_cast<int64_t>( g_atlas_ptr_index.size() ),
        " tile=", tile_width(), "x", tile_height() );
    return ready_;
}

bool MapSnapshot::tileset_ready() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return ready_;
}

std::string MapSnapshot::tileset_id() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return tileset_id_;
}

int MapSnapshot::tile_width() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return tile_w_;
}

int MapSnapshot::tile_height() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return tile_h_;
}

int MapSnapshot::atlas_count() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return static_cast<int>( atlases_.size() );
}

godot::Ref<godot::Image> MapSnapshot::copy_palette_image() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    if( palette_pixels_.w <= 0 || palette_pixels_.h <= 0 ) {
        return {};
    }
    godot::PackedByteArray bytes;
    bytes.resize( static_cast<int64_t>( palette_pixels_.rgba.size() ) );
    std::memcpy( bytes.ptrw(), palette_pixels_.rgba.data(), palette_pixels_.rgba.size() );
    return godot::Image::create_from_data( palette_pixels_.w, palette_pixels_.h, false,
                                           godot::Image::FORMAT_RGBA8, bytes );
}

godot::Ref<godot::Image> MapSnapshot::copy_atlas_image( int index ) const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    if( index < 0 || index >= static_cast<int>( atlases_.size() ) ) {
        return godot::Ref<godot::Image>();
    }
    const atlas_pixels &ap = atlases_[static_cast<size_t>( index )];
    godot::PackedByteArray bytes;
    bytes.resize( static_cast<int64_t>( ap.rgba.size() ) );
    if( !ap.rgba.empty() ) {
        std::memcpy( bytes.ptrw(), ap.rgba.data(), ap.rgba.size() );
    }
    return godot::Image::create_from_data( ap.w, ap.h, false, godot::Image::FORMAT_RGBA8, bytes );
}

void MapSnapshot::release_resources()
{
    std::lock_guard<std::mutex> lock( mutex_ );
    atlases_.clear();
    cmds_.clear();
    ready_ = false;
}

void MapSnapshot::set_requested_view_tiles( const int w, const int h )
{
    req_view_w_.store( std::max( 0, w ), std::memory_order_relaxed );
    req_view_h_.store( std::max( 0, h ), std::memory_order_relaxed );
}

bool MapSnapshot::view_extent_stale() const
{
    const int w = req_view_w_.load( std::memory_order_relaxed );
    const int h = req_view_h_.load( std::memory_order_relaxed );
    if( w <= 0 || h <= 0 ) {
        return false;
    }
    std::lock_guard<std::mutex> lock( mutex_ );
    return std::clamp( w, 11, MAPSIZE_X ) != view_w_ ||
           std::clamp( h, 11, MAPSIZE_Y ) != view_h_;
}

godot::Vector2i MapSnapshot::view_size_tiles() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return godot::Vector2i( view_w_, view_h_ );
}

godot::Vector2i MapSnapshot::view_origin_tiles() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return godot::Vector2i( origin_x_, origin_y_ );
}

int MapSnapshot::command_count() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return static_cast<int>( cmds_.size() );
}

godot::PackedInt32Array MapSnapshot::copy_draw_list() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::PackedInt32Array out;
    out.resize( static_cast<int64_t>( cmds_.size() * cmd_stride ) );
    int32_t *dst = out.ptrw();
    size_t i = 0;
    for( const map_draw_cmd &c : cmds_ ) {
        dst[i++] = c.atlas;
        dst[i++] = c.src_x;
        dst[i++] = c.src_y;
        dst[i++] = c.src_w;
        dst[i++] = c.src_h;
        dst[i++] = c.dest_x;
        dst[i++] = c.dest_y;
        dst[i++] = c.layer;
        dst[i++] = c.tint;
        dst[i++] = c.rot_flags;
    }
    return out;
}

godot::PackedInt32Array MapSnapshot::copy_glyph_list() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::PackedInt32Array out;
    out.resize( static_cast<int64_t>( glyphs_.size() * glyph_stride ) );
    int32_t *dst = out.ptrw();
    size_t i = 0;
    for( const map_glyph_cmd &c : glyphs_ ) {
        dst[i++] = c.dest_x;
        dst[i++] = c.dest_y;
        dst[i++] = c.layer;
        dst[i++] = c.codepoint;
        dst[i++] = c.fg;
        dst[i++] = c.bg;
    }
    return out;
}

godot::PackedInt32Array MapSnapshot::copy_field_list() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::PackedInt32Array out;
    out.resize( static_cast<int64_t>( fields_.size() * field_stride ) );
    int32_t *dst = out.ptrw();
    size_t i = 0;
    for( const map_field_cmd &c : fields_ ) {
        dst[i++] = c.dest_x;
        dst[i++] = c.dest_y;
        dst[i++] = c.kind;
        dst[i++] = c.intensity;
    }
    return out;
}

int MapSnapshot::glyph_count() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return static_cast<int>( glyphs_.size() );
}

godot::Array MapSnapshot::copy_sprite_coverage( const int limit ) const
{
    std::vector<std::pair<const std::string *, const coverage_entry *>> sorted;
    godot::Array out;
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        sorted.reserve( coverage_.size() );
        for( const auto &entry : coverage_ ) {
            sorted.emplace_back( &entry.first, &entry.second );
        }
        std::sort( sorted.begin(), sorted.end(), []( const auto & a, const auto & b ) {
            if( a.second->hits != b.second->hits ) {
                return a.second->hits > b.second->hits;
            }
            // Ties broken by id so repeated calls agree with each other -- an
            // unordered_map's order is otherwise free to change between them.
            return *a.first < *b.first;
        } );
        const size_t take = limit > 0
                            ? std::min( sorted.size(), static_cast<size_t>( limit ) )
                            : sorted.size();
        for( size_t i = 0; i < take; ++i ) {
            godot::Dictionary d;
            d["id"] = godot::String::utf8( sorted[i].first->c_str() );
            const char *cat = category_id( sorted[i].second->category );
            d["category"] = godot::String( cat ? cat : "none" );
            d["level"] = static_cast<int>( sorted[i].second->level );
            d["level_name"] = godot::String( fallback_name( sorted[i].second->level ) );
            d["hits"] = static_cast<int64_t>( sorted[i].second->hits );
            out.push_back( d );
        }
    }
    return out;
}

godot::Dictionary MapSnapshot::copy_render_stats() const
{
    godot::Dictionary out;
    std::lock_guard<std::mutex> lock( mutex_ );
    out["tileset"] = godot::String::utf8( tileset_id_.c_str() );
    out["ready"] = ready_;
    out["atlases"] = static_cast<int>( atlases_.size() );
    out["tile_size"] = godot::Vector2i( tile_w_, tile_h_ );
    out["view_size"] = godot::Vector2i( view_w_, view_h_ );
    out["view_origin"] = godot::Vector2i( origin_x_, origin_y_ );
    out["generation"] = static_cast<int64_t>( generation_ );
    out["commands"] = static_cast<int>( cmds_.size() );
    out["glyphs"] = static_cast<int>( glyphs_.size() );
    out["field_emitters"] = static_cast<int>( fields_.size() );
    out["missing_ids"] = static_cast<int>( coverage_.size() );
    out["palettes"] = palette_pixels_.h > 0 ? palette_pixels_.h - 1 : 0;

    godot::PackedInt32Array by_layer;
    by_layer.resize( static_cast<int64_t>( layer_counts_.size() ) );
    for( size_t i = 0; i < layer_counts_.size(); ++i ) {
        by_layer[static_cast<int64_t>( i )] = layer_counts_[i];
    }
    out["by_layer"] = by_layer;

    godot::Dictionary by_fallback;
    for( size_t i = 0; i < fallback_counts_.size(); ++i ) {
        by_fallback[godot::String( fallback_name( static_cast<sprite_fallback>( i ) ) )] =
            fallback_counts_[i];
    }
    out["by_fallback"] = by_fallback;
    return out;
}

godot::Dictionary MapSnapshot::describe_sprite( const std::string &id,
        const std::string &category ) const
{
    godot::Dictionary out;
    sprite_category cat = sprite_category::none;
    for( int i = 0; i < static_cast<int>( sprite_category::last ); ++i ) {
        const char *name = category_id( static_cast<sprite_category>( i ) );
        if( name && category == name ) {
            cat = static_cast<sprite_category>( i );
            break;
        }
    }

    out["id"] = godot::String::utf8( id.c_str() );
    out["category"] = godot::String::utf8( category.c_str() );

    // The variant redirect is reported separately from the chain: an author
    // saying "draw that sprite instead" is a different fact from the renderer
    // failing to find this one, and conflating them would make the palette
    // feature look like a miss.
    std::string resolved = id;
    const auto var = g_tileset.sprite_variants.find( id );
    if( var != g_tileset.sprite_variants.end() && !var->second.sprite.empty() ) {
        resolved = var->second.sprite;
    }

    const resolved_sprite res = resolve_sprite( id, cat );
    out["resolved"] = godot::String::utf8( resolved.c_str() );
    out["level"] = static_cast<int>( res.level );
    out["level_name"] = godot::String( fallback_name( res.level ) );
    out["palette_row"] = res.palette_row;
    out["codepoint"] = static_cast<int>( res.codepoint );

    bool vegetation = false;
    if( cat == sprite_category::terrain ) {
        const ter_str_id sid( id );
        vegetation = sid.is_valid() && is_vegetation( id, sid.obj() );
    } else if( cat == sprite_category::furniture ) {
        const furn_str_id sid( id );
        vegetation = sid.is_valid() && is_vegetation( id, sid.obj() );
    }
    // Report the decision that is actually made, not half of it: a ground-cell
    // plant is vegetation and still must not shear.
    bool overhangs = false;
    if( res.tile ) {
        const std::vector<int> *picked = res.tile->fg.pick( 0 );
        if( picked && !picked->empty() ) {
            overhangs = sprite_overhangs_cell( ( *picked )[0],
                                               std::max( 1, g_tileset.get_tile_width() ),
                                               std::max( 1, g_tileset.get_tile_height() ) );
        }
    }
    out["vegetation"] = vegetation;
    out["overhangs_cell"] = overhangs;
    out["sways"] = vegetation && overhangs;
    return out;
}

godot::Array MapSnapshot::copy_avatar_overlays() const
{
    godot::Array out;
    std::lock_guard<std::mutex> lock( mutex_ );
    for( const overlay_record &rec : avatar_overlays_ ) {
        godot::Dictionary d;
        d["slot"] = godot::String::utf8( rec.slot.c_str() );
        d["variant"] = godot::String::utf8( rec.variant.c_str() );
        d["sprite"] = godot::String::utf8( rec.sprite.c_str() );
        d["drawn"] = rec.drawn;
        out.push_back( d );
    }
    return out;
}

void MapSnapshot::update_from_game()
{
    if( !g ) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        if( !ready_ ) {
            return;
        }
    }

    map &here = get_map();
    avatar &u = get_avatar();
    const tripoint_bub_ms center = u.pos_bub( here );

    // How much map to publish.
    //
    // This used to come from TERRAIN_WINDOW_WIDTH/HEIGHT -- the *curses* cell
    // grid -- which has nothing to do with the size of the Godot viewport or the
    // zoom it draws at. MapView then letterboxed whatever it was given, so the
    // map appeared as a block of tiles with black bars either side while the
    // minimap showed vision reaching much further. MapView now asks for exactly
    // the tiles its viewport covers; the terminal grid is only the fallback for
    // the first frame, before it has had a chance to.
    int view_w = req_view_w_.load( std::memory_order_relaxed );
    int view_h = req_view_h_.load( std::memory_order_relaxed );
    if( view_w <= 0 || view_h <= 0 ) {
        view_w = TERRAIN_WINDOW_WIDTH > 0 ? TERRAIN_WINDOW_WIDTH : 25;
        view_h = TERRAIN_WINDOW_HEIGHT > 0 ? TERRAIN_WINDOW_HEIGHT : 25;
    }
    // Upper bound is the reality-bubble span: asking for more publishes tiles the
    // map cannot supply, and the cost is per-tile.
    view_w = std::clamp( view_w, 11, MAPSIZE_X );
    view_h = std::clamp( view_h, 11, MAPSIZE_Y );

    const int origin_x = center.x() - view_w / 2;
    const int origin_y = center.y() - view_h / 2;
    const int tw = std::max( 1, g_tileset.get_tile_width() );
    const int th = std::max( 1, g_tileset.get_tile_height() );

    // Match SDL cata_tiles::draw_sprite_at: sprite sheets (e.g. Ultica human_body_plus
    // 32x48 with sprite_offset_y=-16) are anchored via tile_type.offset, not cell origin.
    const int base_tw = std::max( 1, g_tileset.get_tile_width() );

    // Per-frame render statistics and the running coverage report (SP-2).
    // Accumulated locally and merged in one go at the end, so the resolver does
    // not take the snapshot mutex once per tile.
    std::vector<map_glyph_cmd> glyphs;
    std::vector<map_field_cmd> fields;
    std::unordered_map<std::string, coverage_entry> cov;
    std::array<int32_t, static_cast<size_t>( sprite_fallback::last )> fb_counts{};
    std::array<int32_t, 16> layer_counts{};

    auto emit_sprite = [&]( std::vector<map_draw_cmd> &out, int sprite_idx,
    int dest_tx, int dest_ty, map_layer layer, const point &tile_offset, int32_t tint,
    int rot_flags ) {
        if( sprite_idx < 0 || sprite_idx >= static_cast<int>( g_tileset.tile_values.size() ) ) {
            return;
        }
        const godot_texture &tex = g_tileset.tile_values[static_cast<size_t>( sprite_idx )];
        if( !tex.is_valid() ) {
            return;
        }
        const auto it = g_atlas_ptr_index.find( tex.get_texture().ptr() );
        if( it == g_atlas_ptr_index.end() ) {
            return;
        }
        // SDL scales offsets by rendered tile_width / native tile width (both axes
        // use tile_width); at 1:1 present scale this is identity for Ultica 32px.
        const int off_x = divide_round_down( tile_offset.x * tw, base_tw );
        const int off_y = divide_round_down( tile_offset.y * tw, base_tw );
        map_draw_cmd cmd;
        cmd.atlas = it->second;
        cmd.src_x = tex.src_x();
        cmd.src_y = tex.src_y();
        cmd.src_w = tex.src_w();
        cmd.src_h = tex.src_h();
        cmd.dest_x = dest_tx * tw + off_x;
        cmd.dest_y = dest_ty * th + off_y;
        cmd.layer = static_cast<int32_t>( layer );
        cmd.tint = tint;
        cmd.rot_flags = rot_flags;
        const size_t li = static_cast<size_t>( layer );
        if( li < layer_counts.size() ) {
            ++layer_counts[li];
        }
        out.push_back( cmd );
    };

    /// A tile that resolved to nothing drawable, published as its JSON symbol
    /// for MapView's glyph layer to paint (SP-1 step 5).
    auto emit_glyph = [&]( const resolved_sprite & res, int dest_tx, int dest_ty,
    map_layer layer, int32_t tint ) {
        map_glyph_cmd gc;
        gc.dest_x = dest_tx * tw;
        gc.dest_y = dest_ty * th;
        gc.layer = static_cast<int32_t>( layer );
        gc.codepoint = static_cast<int32_t>( res.codepoint );
        // The glyph layer draws with a font rather than through the tile
        // shader, so the lighting modulation has to be folded in here.
        const uint32_t fg = static_cast<uint32_t>( res.glyph_fg );
        const uint32_t tn = static_cast<uint32_t>( tint );
        auto mul = [&]( int shift ) -> uint32_t {
            const uint32_t a = ( fg >> shift ) & 0xFFu;
            const uint32_t b = ( tn >> shift ) & 0xFFu;
            return ( a * b ) / 255u;
        };
        gc.fg = static_cast<int32_t>( ( mul( 24 ) << 24 ) | ( mul( 16 ) << 16 ) |
                                      ( mul( 8 ) << 8 ) | ( fg & 0xFFu ) );
        glyphs.push_back( gc );
    };

    auto emit_tile_id = [&]( std::vector<map_draw_cmd> &out, const std::string &id,
    sprite_category cat, int dest_tx, int dest_ty, map_layer fg_layer, bool with_bg,
    const light_tints &tint, unsigned int seed, int rotation = 0, bool sway = false,
    bool flip_x = false ) {
        const resolved_sprite res = resolve_sprite( id, cat );
        ++fb_counts[static_cast<size_t>( res.level )];
        if( res.level != sprite_fallback::exact ) {
            coverage_entry &ce = cov[id];
            ce.category = cat;
            // Keep the worst level seen: the same id can resolve exactly as one
            // subtile and by glyph as another, and the miss is what matters.
            if( static_cast<int>( res.level ) > static_cast<int>( ce.level ) ) {
                ce.level = res.level;
            }
            ++ce.hits;
        }
        if( res.level == sprite_fallback::glyph ) {
            emit_glyph( res, dest_tx, dest_ty, fg_layer, tint.glyph );
            return;
        }
        const godot_tile_type *tt = res.tile;
        if( !tt ) {
            return;
        }
        // A fallback sprite is a stand-in, not the tileset's own orientation of
        // this tile: turning it produces a sideways question mark. Line-drawing
        // characters already encode their direction in the symbol. Same rule as
        // cata_tiles, which zeroes rota for every non-linear fallback.
        if( res.level == sprite_fallback::ascii || res.level == sprite_fallback::category ) {
            rotation = 0;
        }
        const int32_t palette_bits =
            ( ( res.palette_row & 0xF ) << cmd_palette_shift ) |
            ( flip_x ? cmd_flag_flip_x : 0 );
        // A picked variant is already the right orientation, so it is drawn
        // square. Only a lone sprite gets turned -- and then by a quarter turn,
        // because rotation can be 0-15 when the tileset offers 8 or 16 variants
        // and only the low two bits are an angle. cata_tiles does the same
        // `rota % 4` before choosing an angle.
        if( with_bg ) {
            const picked_sprite bg = pick_sprite_rota( tt->bg, seed + 1, false, rotation );
            // The background is the ground the plant stands on. Swaying it too
            // would shear the floor.
            emit_sprite( out, bg.index, dest_tx, dest_ty, map_layer::terrain_bg,
                         tt->offset, tint.sprite,
                         ( bg.rotate ? rotation % 4 : 0 ) | palette_bits );
        }
        const picked_sprite fg = pick_sprite_rota( tt->fg, seed, true, rotation );
        // Being vegetation is not enough to sway: the sprite also has to be one
        // that can move without tearing. See sprite_overhangs_cell.
        const bool shears = sway && sprite_overhangs_cell( fg.index, tw, th );
        emit_sprite( out, fg.index, dest_tx, dest_ty, fg_layer, tt->offset, tint.sprite,
                     ( fg.rotate ? rotation % 4 : 0 ) | palette_bits |
                     ( shears ? cmd_flag_sway : 0 ) );
    };

    /**
     * Pick the sprite id and rotation for a tile that connects to its neighbours.
     *
     * Walls, fences, roads and pipes are drawn from multitile subtiles registered as
     * "<id>_<subtile>"; without this every one of them draws its unconnected sprite
     * and nothing joins up. Falls back to the base id when the tileset has no
     * subtile for this shape, which is also what SDL does.
     */
    auto connected_id = [&]( const std::string & base, const uint8_t connections,
    const char rot_to, int &rotation ) -> std::string {
        int subtile = 0;
        connections_to_subtile( connections, rot_to, subtile, rotation );
        if( const char *name = subtile_name( subtile ) ) {
            const std::string candidate = base + "_" + name;
            if( find_tile_by_id_exact( candidate ) ) {
                return candidate;
            }
        }
        rotation = 0;
        return base;
    };

    std::vector<map_draw_cmd> cmds;
    cmds.reserve( static_cast<size_t>( view_w * view_h * 3 ) );

    const bool nv_goggles = u.has_nv_goggles();
    // Lighting goes out as a texture over the same extent (SP-3, SP-4). While
    // the Godot side is running that pass the per-sprite tints carry hue only.
    LightSnapshot &lights = get_light_snapshot();
    const bool light_pass = lights.pass_enabled();
    lights.begin( view_w, view_h );
    lights.set_wind( static_cast<float>( get_weather().winddirection ),
                     static_cast<float>( get_weather().windspeed ) );

    for( int ty = 0; ty < view_h; ++ty ) {
        for( int tx = 0; tx < view_w; ++tx ) {
            const tripoint_bub_ms p( origin_x + tx, origin_y + ty, center.z() );
            if( !here.inbounds( p ) ) {
                continue;
            }
            const unsigned int seed = simple_point_hash( here.get_abs( p ).raw().xy() );

            if( !u.sees( here, p, true ) ) {
                // Not currently visible: draw what the character remembers rather
                // than leaving a hole. Previously the whole tile was skipped, so
                // anything out of sight simply vanished.
                const memorized_tile &mem = u.get_memorized_tile( here.get_abs( p ) );
                const light_tints mem_tint =
                    tints_for_light( lit_level::MEMORIZED, false, light_pass );
                // Remembered, not seen -- but only where there is actually a
                // memory. Marking every unseen tile as remembered would tell the
                // shader that the whole unexplored map is somewhere the player
                // has been, and LightSnapshot::begin has already cleared this to
                // "never seen".
                if( !mem.get_ter_id().empty() || !mem.get_dec_id().empty() ) {
                    lights.set( tx, ty, LightSnapshot::vis_remembered, 0 );
                }
                if( !mem.get_ter_id().empty() ) {
                    emit_tile_id( cmds, mem.get_ter_id(), sprite_category::terrain, tx, ty,
                                  map_layer::terrain_fg, true,
                                  mem_tint, seed );
                }
                if( !mem.get_dec_id().empty() ) {
                    emit_tile_id( cmds, mem.get_dec_id(), sprite_category::furniture, tx, ty,
                                  map_layer::furniture, false,
                                  mem_tint, seed );
                }
                continue;
            }

            const lit_level ll = here.light_at( p );
            lights.set( tx, ty, LightSnapshot::vis_seen,
                        encode_light_level( ll, here.ambient_light_at( p ) ) );
            const light_tints tint = tints_for_light( ll, nv_goggles, light_pass );

            const ter_id tid = here.ter( p );
            if( tid ) {
                std::string ter_sprite = tid.id().str();
                int ter_rotation = 0;
                const std::bitset<NUM_TERCONN> &ter_connect = tid.obj().connect_to_groups;
                if( ter_connect.any() ) {
                    // rotates_to is what makes a road turn to face the verge it
                    // runs alongside; with no group, get_known_rotates_to returns
                    // CHAR_MAX and the no-rotation path applies.
                    ter_sprite = connected_id( ter_sprite,
                                               here.get_known_connections( p, ter_connect ),
                                               static_cast<char>( here.get_known_rotates_to(
                                                       p, tid.obj().rotate_to_groups ) ),
                                               ter_rotation );
                }
                emit_tile_id( cmds, ter_sprite, sprite_category::terrain, tx, ty,
                              map_layer::terrain_fg, true, tint, seed,
                              ter_rotation, is_vegetation( tid.id().str(), tid.obj() ) );
            }

            const furn_id fid = here.furn( p );
            if( fid ) {
                // Movable furniture keeps seed 0 so dragging it does not make its
                // sprite flicker between variants, matching cata_tiles.
                const unsigned int furn_seed = fid.obj().is_movable() ? 0u : seed;
                std::string furn_sprite = fid.id().str();
                int furn_rotation = 0;
                const std::bitset<NUM_TERCONN> &furn_connect = fid.obj().connect_to_groups;
                if( furn_connect.any() ) {
                    furn_sprite = connected_id( furn_sprite,
                                                here.get_known_connections_f( p, furn_connect ),
                                                static_cast<char>( here.get_known_rotates_to_f(
                                                        p, fid.obj().rotate_to_groups, {}, {} ) ),
                                                furn_rotation );
                }
                emit_tile_id( cmds, furn_sprite, sprite_category::furniture, tx, ty,
                              map_layer::furniture, false, tint,
                              furn_seed, furn_rotation,
                              is_vegetation( fid.id().str(), fid.obj() ) );
            }

            const trap &tr = here.tr_at( p );
            if( !tr.is_null() && here.can_see_trap_at( p, u ) ) {
                emit_tile_id( cmds, tr.id.str(), sprite_category::trap, tx, ty, map_layer::trap,
                              false, tint, seed );
            }

            // Fields: fire, smoke, gas clouds. Intensity picks a distinct sprite
            // where the tileset provides one ("<id>_int<N>"), as in cata_tiles.
            const field &fld = here.field_at( p );
            const field_type_id displayed = fld.displayed_field_type();
            if( displayed ) {
                const int intensity = fld.displayed_intensity();
                const std::string base = displayed.id().str();
                bool drawn = false;
                if( intensity > 0 ) {
                    const std::string with_int = base + "_int" + std::to_string( intensity );
                    if( find_tile_by_id_exact( with_int ) ) {
                        emit_tile_id( cmds, with_int, sprite_category::field, tx, ty, map_layer::field,
                                      false, tint, seed );
                        drawn = true;
                    }
                }
                if( !drawn ) {
                    emit_tile_id( cmds, base, sprite_category::field, tx, ty, map_layer::field,
                                  false, tint, seed );
                }
                // Anything that burns or drifts also gets particles (SP-6).
                // Decided from the field type rather than from a list of ids, so
                // a mod's own smoke behaves like smoke without being enumerated.
                const field_type &ftype = displayed.obj();
                if( ftype.has_fire || ftype.phase == phase_id::GAS ) {
                    map_field_cmd fc;
                    fc.dest_x = tx * tw;
                    fc.dest_y = ty * th;
                    fc.kind = static_cast<int32_t>( ftype.has_fire ? field_particle::fire
                                                    : field_particle::smoke );
                    fc.intensity = std::max( 1, intensity );
                    fields.push_back( fc );
                    if( ftype.has_fire ) {
                        // Feeds the flicker the tile shader adds around a fire.
                        // Intensity 3 is a wall of flame; 1 is a burning scrap.
                        lights.set_fire( tx, ty, static_cast<uint8_t>(
                                             std::min( 255, 90 + 55 * fc.intensity ) ) );
                    }
                }
            }

            // Vehicles. vpart_display already resolves which part of a stack is
            // the visible one, including its tileset id.
            if( const optional_vpart_position ovp = here.veh_at( p ) ) {
                const vpart_display vd = ovp->vehicle().get_display_of_tile( ovp->mount_pos() );
                if( !vd.id.is_null() ) {
                    // get_tileset_id() already yields "vp_<id>_<variant>" (or
                    // "vp_<id>" when there is no variant), so variants are picked
                    // up for free. Seeded by position within the vehicle, so a
                    // part keeps its sprite as the vehicle drives (as cata_tiles).
                    emit_tile_id( cmds, vd.get_tileset_id(), sprite_category::vehicle_part, tx, ty,
                                  map_layer::vehicle, false,
                                  tint, simple_point_hash( ovp->mount_pos().raw() ) );
                }
            }

            // Draw the whole visible stack, not just the first item: a pile of
            // loot rendered as one sprite hid everything under it.
            map_stack items = here.i_at( p );
            for( const item &it : items ) {
                emit_tile_id( cmds, it.typeId().str(), sprite_category::item, tx, ty, map_layer::item,
                              false, tint, seed );
            }
        }
    }

    /**
     * Clothing, the weapon in hand, mutations and effects, over the body.
     *
     * Character::get_overlay_ids already returns them in draw order -- effects,
     * then mutations sorted by overlay_ordering, then bionics, then worn items
     * innermost-first, then the wielded weapon. That ordering is game data and
     * this must not second-guess it, so the list is emitted in the order given
     * and MapView is what has to preserve it.
     */
    std::vector<overlay_record> avatar_overlays;
    auto emit_overlays = [&]( const std::vector<std::pair<std::string, std::string>> &overlays,
    bool male, int tx, int ty, map_layer layer, const light_tints & tint, bool flip,
    bool record = false ) {
        for( const std::pair<std::string, std::string> &overlay : overlays ) {
            std::string draw_id;
            const bool found = resolve_overlay_id( male, overlay.first, overlay.second, draw_id );
            if( record ) {
                avatar_overlays.push_back( { overlay.first, overlay.second,
                                             found ? draw_id : std::string(), found } );
            }
            if( !found ) {
                // No art for this garment in this tileset. Nothing to draw and
                // nothing wrong: a tileset covers a fraction of the item list,
                // and a missing coat must not become a question mark on the
                // character's chest.
                continue;
            }
            emit_tile_id( cmds, draw_id, sprite_category::character, tx, ty, layer, false,
                          tint, 0, 0, false, flip );
        }
    };

    /// A body plus everything worn on it. Used for the avatar and for NPCs,
    /// which were not drawn at all before this.
    auto emit_character = [&]( const Character & ch, int tx, int ty, map_layer body_layer,
    map_layer overlay_layer, const light_tints & tint, bool record = false ) {
        // SDL mirrors the sprite for a left-facing character rather than
        // rotating it. FacingDirection::NONE draws unflipped: skipping it, as
        // cata_tiles does, would make the character invisible.
        const bool flip = ch.facing == FacingDirection::LEFT;
        const char *body = ch.is_npc() ? ( ch.male ? "npc_male" : "npc_female" )
                           : ( ch.male ? "player_male" : "player_female" );
        emit_tile_id( cmds, body, sprite_category::character, tx, ty, body_layer, false,
                      tint, 0, 0, false, flip );
        emit_overlays( ch.get_overlay_ids(), ch.male, tx, ty, overlay_layer, tint, flip,
                       record );
    };

    for( Creature &critter : g->all_creatures() ) {
        if( critter.is_avatar() ) {
            continue;
        }
        const tripoint_bub_ms p = critter.pos_bub( here );
        const int tx = p.x() - origin_x;
        const int ty = p.y() - origin_y;
        if( tx < 0 || ty < 0 || tx >= view_w || ty >= view_h ) {
            continue;
        }
        if( !u.sees( here, critter ) ) {
            continue;
        }
        const light_tints critter_tint =
            tints_for_light( here.light_at( p ), nv_goggles, light_pass );
        if( const monster *mon = dynamic_cast<const monster *>( &critter ) ) {
            // cata_tiles leaves the seed at 0 for creatures (its switch has no
            // MONSTER case), so a position hash here would disagree with SDL.
            emit_tile_id( cmds, mon->type->id.str(), sprite_category::monster, tx, ty,
                          map_layer::monster, false, critter_tint, 0, 0, false,
                          mon->facing == FacingDirection::LEFT );
            // Monsters carry overlays too: effects, and the body-type variants
            // a rideable animal uses for its saddle.
            emit_overlays( mon->get_overlay_ids(), true, tx, ty, map_layer::monster_overlay,
                           critter_tint, mon->facing == FacingDirection::LEFT );
        } else if( const Character *ch = dynamic_cast<const Character *>( &critter ) ) {
            // NPCs reached this loop and fell out of it: only the monster cast
            // was handled, so every NPC in the game was invisible.
            emit_character( *ch, tx, ty, map_layer::monster, map_layer::monster_overlay,
                            critter_tint );
        }
    }

    {
        const int tx = center.x() - origin_x;
        const int ty = center.y() - origin_y;
        // The avatar is always fully lit: it is the viewpoint, and dimming it
        // makes it hard to find on screen in the dark.
        const light_tints lit{ pack_tint( 255, 255, 255, 255 ),
                               pack_tint( 255, 255, 255, 255 ) };
        if( g_tileset.find_tile_type( u.male ? "player_male" : "player_female" ) ) {
            emit_character( u, tx, ty, map_layer::player, map_layer::player_overlay, lit,
                            /*record=*/true );
        } else {
            // A tileset with no character art at all. Draw something rather
            // than nothing, and skip the overlays: they would be stacked on a
            // question mark.
            const char *fallback = g_tileset.find_tile_type( "player" ) ? "player" : "unknown";
            emit_tile_id( cmds, fallback, sprite_category::character, tx, ty,
                          map_layer::player, false, lit, 0 );
        }
    }

    // stable_sort, not sort: a character's overlays are emitted in slot order
    // immediately after the body, and they tie on every key here. std::sort is
    // free to shuffle equal elements, which would put the coat under the shirt
    // on an arbitrary frame.
    std::stable_sort( cmds.begin(), cmds.end(),
    []( const map_draw_cmd & a, const map_draw_cmd & b ) {
        if( a.layer != b.layer ) {
            return a.layer < b.layer;
        }
        if( a.dest_y != b.dest_y ) {
            return a.dest_y < b.dest_y;
        }
        return a.dest_x < b.dest_x;
    } );

    lights.blur_fire();
    lights.commit();

    std::lock_guard<std::mutex> lock( mutex_ );
    cmds_ = std::move( cmds );
    glyphs_ = std::move( glyphs );
    fields_ = std::move( fields );
    avatar_overlays_ = std::move( avatar_overlays );
    fallback_counts_ = fb_counts;
    layer_counts_ = layer_counts;
    // Coverage accumulates across the session rather than per frame: what the
    // report is for is "which missing ids has this player actually looked at",
    // and one frame of a corridor answers that badly.
    for( const auto &entry : cov ) {
        coverage_entry &dst = coverage_[entry.first];
        dst.category = entry.second.category;
        if( static_cast<int>( entry.second.level ) > static_cast<int>( dst.level ) ) {
            dst.level = entry.second.level;
        }
        dst.hits += entry.second.hits;
    }
    view_w_ = view_w;
    view_h_ = view_h;
    origin_x_ = origin_x;
    origin_y_ = origin_y;
    ++generation_;
}

uint64_t MapSnapshot::generation() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return generation_;
}

} // namespace godot_backend

#endif // GODOT
