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
#include "game_constants.h"
#include "map_scale_constants.h"
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
#include "units_utility.h"
#include "vehicle.h"
#include "vpart_position.h"
#include "weather.h"
#include "weather_type.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
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
    /// The tileset id that actually matched. Worth carrying because "exact" on
    /// its own cannot distinguish a correct longest-variant match from a lucky
    /// short one -- vp_frame_horizontal_2_front and vp_frame_horizontal are
    /// both "exact" and only one of them is right.
    std::string matched_id;
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

/// How far a sprite standing in front of the avatar is allowed to fade, as the
/// alpha it reaches at full occlusion. Not zero: the tree has to stay visible
/// enough to be a tree.
constexpr int32_t min_occluder_alpha = 90;

/**
 * How many cells a sprite reaches over, as (columns, rows).
 *
 * Sprites are anchored to the bottom of their cell and centred across it, so a
 * 32x96 sprite at row R covers rows R-2..R and a 96x32 one covers columns
 * C-1..C+1. Used to answer "does this actually cover the avatar" rather than
 * inferring it from distance, which fades things that are not in the way.
 */
point sprite_cells_covered( const int sprite_idx, const int tile_w, const int tile_h )
{
    if( sprite_idx < 0 || sprite_idx >= static_cast<int>( g_tileset.tile_values.size() ) ) {
        return point( 1, 1 );
    }
    const godot_texture &tex = g_tileset.tile_values[static_cast<size_t>( sprite_idx )];
    if( !tex.is_valid() ) {
        return point( 1, 1 );
    }
    const int w = std::max( 1, tile_w );
    const int h = std::max( 1, tile_h );
    return point( ( tex.src_w() + w - 1 ) / w, ( tex.src_h() + h - 1 ) / h );
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
/**
 * Try @p id with @p variant applied, the way this category spells variants.
 *
 * Two conventions, both from cata_tiles::find_tile_looks_like:
 *
 * - Vehicle parts join with a plain underscore and are matched greedily, longest
 *   first: "vp_frame" with variant "horizontal_2_front" tries
 *   vp_frame_horizontal_2_front, then _horizontal_2, then _horizontal, then the
 *   bare part. Ultica is built to that -- it ships vp_frame_horizontal_2_front
 *   and vp_frame_horizontal as separate sprites.
 * - Everything else uses "_var_", or appends directly when the variant already
 *   begins with an underscore.
 */
const godot_tile_type *find_variant( const std::string &id, const std::string &variant,
                                     const sprite_category cat, std::string &matched )
{
    if( variant.empty() ) {
        return nullptr;
    }
    if( cat == sprite_category::vehicle_part ) {
        std::string chunk = variant;
        while( !chunk.empty() ) {
            const std::string candidate = id + "_" + chunk;
            if( const godot_tile_type *t = g_tileset.find_tile_type( candidate ) ) {
                matched = candidate;
                return t;
            }
            const size_t cut = chunk.rfind( '_' );
            chunk = cut == std::string::npos ? std::string() : chunk.substr( 0, cut );
        }
        return nullptr;
    }
    const std::string candidate = variant.front() == '_' ? id + variant
                                  : id + "_var_" + variant;
    if( const godot_tile_type *t = g_tileset.find_tile_type( candidate ) ) {
        matched = candidate;
        return t;
    }
    return nullptr;
}

resolved_sprite resolve_sprite( const std::string &id, const sprite_category cat,
                                const std::string &variant = {} )
{
    resolved_sprite out;
    if( id.empty() ) {
        return out;
    }

    // A variant is more specific than the base id, so it is tried first -- and
    // it counts as an exact hit, because the tileset does have art for exactly
    // this thing.
    std::string matched;
    if( const godot_tile_type *t = find_variant( id, variant, cat, matched ) ) {
        out.tile = t;
        out.level = sprite_fallback::exact;
        out.matched_id = matched;
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
        out.matched_id = lookup;
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
            out.matched_id = found;
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
        case open_: return "open";
        case broken: return "broken";
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

/**
 * Push a tint one z-level's worth further away (ADR-005 item 1).
 *
 * SDL draws a translucent rectangle over every level below the viewer --
 * cata_tiles::draw_zlevel_overlay, alpha 100/255 on a non-isometric tileset --
 * so distance reads as haze. There is no geometry pass here and the per-command
 * tint is a multiply, which can take light out of a sprite but cannot wash it
 * toward grey. So the same idea arrives as a darkening with a cool cast, which
 * is the half of the effect that carries in a fixed-angle view: the floor of the
 * basement you are looking down into should be dimmer than the floor you stand
 * on, and dimmer again two levels down.
 *
 * Compounding per level, as SDL's does -- it draws the overlay once per level
 * crossed, so two levels down is fogged twice.
 *
 * Alpha is untouched. Fading the lower level instead of dimming it would show
 * the void through it, which is not what is under a floor.
 */
int32_t fog_for_depth( const int32_t tint, const int z_below )
{
    if( z_below <= 0 ) {
        return tint;
    }
    // A renderer that puts levels at real elevations fades them itself, and doing both
    // would dim a basement twice. Same handshake as the light pass: silence means the
    // baked fog stays, so the 2D backend never notices this exists.
    if( get_light_snapshot().depth_fog_enabled() ) {
        return tint;
    }
    // Blue kept highest of the three: what is left of the light down a stairwell
    // is bounced and cold.
    constexpr float per_level[3] = { 0.55f, 0.58f, 0.66f };
    const uint32_t packed = static_cast<uint32_t>( tint );
    int channel[3];
    for( int i = 0; i < 3; ++i ) {
        float value = static_cast<float>( ( packed >> ( 24 - i * 8 ) ) & 0xFFu );
        for( int step = 0; step < z_below; ++step ) {
            value *= per_level[i];
        }
        channel[i] = static_cast<int>( value );
    }
    return pack_tint( channel[0], channel[1], channel[2],
                      static_cast<int>( packed & 0xFFu ) );
}

light_tints fog_for_depth( const light_tints &tints, const int z_below )
{
    return { fog_for_depth( tints.sprite, z_below ),
             fog_for_depth( tints.glyph, z_below ) };
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
    // Light sources published this frame (ADR-006 item 3D-2). A consumption
    // signal, in the tradition of `retracted` and `faded` below: these come out of
    // state level_cache.h describes as valid only inside generate_lightmap, so a
    // counter stuck at zero on a lit night is how that contract breaking would
    // announce itself instead of the lights quietly never arriving.
    out["lights"] = get_light_snapshot().light_count();
    out["retracted"] = retracted_count_;
    out["transparent"] = transparent_count_;
    out["faded"] = faded_count_;
    out["tall_candidates"] = tall_candidates_;
    // ADR-005 item 1. open_columns is the one that answers "what did z-levels
    // cost": it is how many of the view's columns had no floor and made the walk
    // descend, and on an outdoor level it is zero.
    out["open_columns"] = open_columns_;
    out["deepest_z_below"] = deepest_z_below_;
    out["below_commands"] = below_cmds_;
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
        const std::string &category, const std::string &variant ) const
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

    const resolved_sprite res = resolve_sprite( id, cat, variant );
    out["variant"] = godot::String::utf8( variant.c_str() );
    out["resolved"] = godot::String::utf8( resolved.c_str() );
    // The sprite that was actually chosen, which is the only thing that says
    // whether the variant walk landed where it should have.
    out["matched"] = godot::String::utf8( res.matched_id.c_str() );
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

godot::Array MapSnapshot::copy_creatures() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::Array out;
    for( const creature_record &rec : creatures_ ) {
        godot::Dictionary d;
        d["id"] = godot::String::utf8( rec.id.c_str() );
        d["kind"] = rec.kind;
        d["x"] = rec.x;
        d["y"] = rec.y;
        d["z_below"] = rec.z_below;
        d["flip"] = rec.flip;
        out.push_back( d );
    }
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

    /**
     * How much a tall sprite should duck out of the player's way, 0-100.
     *
     * A 96x96 tree drawn at the tile in front of the avatar covers the avatar.
     * Tilesets solve this by shipping a second, shorter offset per sprite
     * (offset_retracted) and a transparent variant, and by declaring the
     * distance band over which to blend between them. Ultica ships all of it;
     * the renderer has been parsing it and throwing it away since the port
     * began, so tall things simply hid the player.
     *
     * Mirrors cata_tiles::draw_from_id_string_internal: option 0 disables it,
     * 1 retracts always, 2 blends over the distance band, with the option's
     * band overriding the tileset's when set.
     */
    const int occlusion_mode = get_option<int>( "PREVENT_OCCLUSION" );
    const bool occlusion_transp = get_option<bool>( "PREVENT_OCCLUSION_TRANSP" );
    const bool occlusion_retract = get_option<bool>( "PREVENT_OCCLUSION_RETRACT" );
    const float opt_d_min = get_option<float>( "PREVENT_OCCLUSION_MIN_DIST" );
    const float opt_d_max = get_option<float>( "PREVENT_OCCLUSION_MAX_DIST" );

    auto retract_at = [&]( int dest_tx, int dest_ty ) -> int {
        if( occlusion_mode == 0 || ( !occlusion_transp && !occlusion_retract ) )
        {
            return 0;
        }
        if( occlusion_mode == 1 ) {
            return 100;
        }
        const float d_min = opt_d_min > 0.0f ? opt_d_min
                            : g_tileset.get_prevent_occlusion_min_dist();
        const float d_max = opt_d_max > 0.0f ? opt_d_max
                            : g_tileset.get_prevent_occlusion_max_dist();
        const float range = d_max - d_min;
        const float slope = range <= 0.0f ? 100.0f : 1.0f / range;
        // Distance from the avatar's tile, which is the centre of the view.
        const float dx = static_cast<float>( dest_tx - ( center.x() - origin_x ) );
        const float dy = static_cast<float>( dest_ty - ( center.y() - origin_y ) );
        const float distance = std::sqrt( dx * dx + dy * dy );
        return static_cast<int>( 100.0f * ( 1.0f -
                                            std::clamp( ( distance - d_min ) * slope, 0.0f, 1.0f ) ) );
    };

    // The avatar's tile in view coordinates. Everything below asks questions
    // about what is standing in front of it.
    const int avatar_tx = center.x() - origin_x;
    const int avatar_ty = center.y() - origin_y;

    /**
     * Does this sprite actually cover the avatar?
     *
     * Distance alone is what cata_tiles uses, and it is the right test for
     * retraction -- a sprite that ducks looks fine ducking whether or not it
     * was in the way. It is the wrong test for a fade, which is visible: it
     * would put a halo of half-transparent trees around the player, including
     * behind them where nothing is being hidden.
     *
     * Only something drawn *in front of* the avatar can hide it, now that the
     * depth order draws everything a row further back first.
     */
    auto covers_avatar = [&]( const int dest_tx, const int dest_ty, const int sprite_idx ) -> bool {
        if( dest_ty <= avatar_ty )
        {
            return false;
        }
        const point cells = sprite_cells_covered( sprite_idx, tw, th );
        // Reaches back far enough to still be over the avatar's row?
        if( dest_ty - avatar_ty >= cells.y ) {
            return false;
        }
        return std::abs( dest_tx - avatar_tx ) <= cells.x / 2;
    };

    // Per-frame render statistics and the running coverage report (SP-2).
    // Accumulated locally and merged in one go at the end, so the resolver does
    // not take the snapshot mutex once per tile.
    std::vector<map_glyph_cmd> glyphs;
    std::vector<map_field_cmd> fields;
    std::unordered_map<std::string, coverage_entry> cov;
    std::array<int32_t, static_cast<size_t>( sprite_fallback::last )> fb_counts{};
    std::array<int32_t, 16> layer_counts{};
    int32_t retracted = 0;
    int32_t transparent = 0;
    int32_t faded = 0;
    int32_t tall_candidates = 0;

    /**
     * Which z-level the emitters below are currently working on, as levels below
     * the avatar's (ADR-005 item 1).
     *
     * Set by the column walk, read by emit_sprite, and zero everywhere else --
     * including for the creature pass, which sets it per creature. A level is
     * hundreds of sprites and a dozen emit sites, and every one of them would
     * have had to carry the same constant argument to avoid this; the failure
     * mode of missing one is a sprite drawn at the wrong depth, which reads as a
     * sorting bug rather than as a dropped parameter.
     */
    int cur_z_below = 0;

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
        cmd.rot_flags = rot_flags |
                        ( ( std::clamp( cur_z_below, 0, max_z_below ) << cmd_z_below_shift ) &
                          cmd_z_below_mask );
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
    bool flip_x = false, const std::string &variant = {} ) {
        const resolved_sprite res = resolve_sprite( id, cat, variant );
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
        const std::vector<int> *first_fg = tt->fg.pick( 0 );
        const bool overhangs = first_fg && !first_fg->empty() &&
                               sprite_overhangs_cell( ( *first_fg )[0], tw, th );
        const int32_t palette_bits =
            ( ( res.palette_row & 0xF ) << cmd_palette_shift ) |
            ( flip_x ? cmd_flag_flip_x : 0 ) |
            ( overhangs ? cmd_flag_tall : 0 );

        // Occlusion handling. Only sprites that overhang their cell can hide
        // anything, so only they are considered -- the same test the sway flag
        // uses, and for a related reason.
        point draw_offset = tt->offset;
        const godot_tile_type *draw_tt = tt;
        /// 255 unless this sprite is standing in front of the avatar; see below.
        int32_t draw_alpha = 255;
        // Creatures never retract. cata_tiles passes retract = 0 explicitly when
        // drawing a character, and the reason shows up immediately without it:
        // the avatar is a 32x48 sprite standing at distance zero from itself, so
        // it and every one of its overlays ducked to avoid occluding the player
        // they *are*. The counter caught it as 12 retracted sprites in an empty
        // shelter -- one body and eleven pieces of clothing.
        const bool is_creature = cat == sprite_category::character ||
                                 cat == sprite_category::monster;
        // Nothing on a lower level can hide the avatar: the whole level draws
        // under it. Considering them anyway would fade trees at the bottom of a
        // pit for standing in front of someone two floors above them, and would
        // put those in tall_candidates, where the number is there to say whether
        // the fade ever gets a chance to fire.
        if( occlusion_mode != 0 && !is_creature && cur_z_below == 0 &&
            res.level == sprite_fallback::exact ) {
            const bool tall = overhangs;
            // Nothing to retract *to*. The loader defaults offset_retracted to
            // offset, so a tileset that declares no retracted offsets -- Ultica
            // declares none -- makes the blend a no-op, and with no
            // "_transparent" variant either there is no way for this to change
            // what is drawn. Skipping outright keeps it from being suspected
            // when something else is wrong, and costs nothing when it could
            // have worked.
            const bool can_retract = tt->offset_retracted != tt->offset ||
                                     find_tile_by_id_exact( id + "_transparent" );
            if( tall && can_retract ) {
                const int retract = retract_at( dest_tx, dest_ty );
                if( retract > 0 ) {
                    ++retracted;
                    if( occlusion_retract ) {
                        draw_offset = retract >= 100
                                      ? tt->offset_retracted
                                      : tt->offset +
                                      ( ( tt->offset_retracted - tt->offset ) * retract ) / 100;
                    }
                    // Ultica ships 44 "_transparent" sprites for exactly this.
                    if( occlusion_transp ) {
                        if( const godot_tile_type *clear =
                                g_tileset.find_tile_type( id + "_transparent" ) ) {
                            draw_tt = clear;
                            ++transparent;
                        }
                    }
                }
            } else if( tall ) {
                // The tileset has nothing to retract to. That used to mean the
                // whole mechanism was inert and tall things simply never got
                // out of the way -- harmless while the draw order put every
                // creature over every tree anyway, and not harmless now that
                // depth ordering lets a tree in front of the avatar cover it.
                // A player hidden behind a trunk is worse than a player pasted
                // in front of one.
                //
                // So the same policy is applied through the one channel that is
                // always available: the sprite's own alpha, which the tint
                // already carries and the shader already multiplies in. Same
                // 0-100 from retract_at, so the game's PREVENT_OCCLUSION
                // options and the tileset's distance band still decide when and
                // how much. Only the mechanism differs, and only because the
                // art cannot do the intended one.
                // Deliberately not gated on retract_at, which every other path
                // here uses. That distance band exists to *guess* whether a
                // sprite is in the way without doing the geometry, and
                // UltimateCataclysm declares it as (-1, 1) -- which works out
                // to a retraction of 50% on the avatar's own tile and exactly
                // zero one tile away, in SDL as much as here. Gating on it
                // would have made this inert for the same reason retraction is
                // inert, and for the same underlying reason: a tileset value
                // nobody had read.
                //
                // The band is also answering a question we no longer have to
                // ask. It was tuned against a renderer that drew every creature
                // over every tree, so nothing was ever really occluded and
                // there was nothing to tune it against. covers_avatar knows.
                //
                // PREVENT_OCCLUSION still decides whether any of this happens
                // at all -- it is off at 0, which is the setting for "I want to
                // see the walls" -- but On and Auto collapse to the same thing,
                // because coverage *is* the automatic condition.
                const std::vector<int> *fg0 = tt->fg.pick( 0 );
                // Counted before the geometry test, not after. "0 faded" on its
                // own cannot tell "nothing was standing in front of the avatar"
                // from "the test never fires", and those want opposite
                // responses. With the candidates alongside it, zero of zero is
                // a scene with nothing tall in it and zero of many is a bug.
                ++tall_candidates;
                if( fg0 && !fg0->empty() && covers_avatar( dest_tx, dest_ty, ( *fg0 )[0] ) ) {
                    ++faded;
                    // Never to nothing. A tree you cannot see is a tree you
                    // walk into, and the point is to read the character
                    // through the canopy, not to delete it.
                    draw_alpha = min_occluder_alpha;
                }
            }
        }
        tt = draw_tt;
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
                         draw_offset, tint.sprite,
                         ( bg.rotate ? rotation % 4 : 0 ) | palette_bits );
        }
        const picked_sprite fg = pick_sprite_rota( tt->fg, seed, true, rotation );
        // Being vegetation is not enough to sway: the sprite also has to be one
        // that can move without tearing. See sprite_overhangs_cell.
        const bool shears = sway && sprite_overhangs_cell( fg.index, tw, th );
        // Only the foreground fades. The background is the ground the tall thing
        // stands on, and fading the floor would show the void through it.
        // Multiplied into the tint's alpha rather than replacing it. Every
        // lighting tint is opaque today, so the two are the same thing; they
        // stop being the same thing the moment one is not.
        emit_sprite( out, fg.index, dest_tx, dest_ty, fg_layer, draw_offset,
                     ( tint.sprite & ~0xFF ) | ( ( ( tint.sprite & 0xFF ) * draw_alpha ) / 255 ),
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
    {
        // Conditions for the presentation grade. All of this is computed every
        // turn already and none of it reached the screen before.
        LightSnapshot::conditions cond;
        // Against default_daylight_level, not max_sun_irradiance. The two are
        // different units -- sun_light_at returns the game's light scale, which
        // peaks around 100, while max_sun_irradiance is 1000 W/m2. Dividing by
        // the wrong one put 8am at 0.0996 and would have graded a spring
        // morning as very nearly night.
        const float full_day = std::max( 1.0f, default_daylight_level() );
        cond.daylight = std::clamp( sun_light_at( calendar::turn ) / full_day, 0.0f, 1.0f );
        switch( get_weather().weather_id->precip ) {
            case precip_class::very_light: cond.precipitation = 0.33f; break;
            case precip_class::light:      cond.precipitation = 0.66f; break;
            case precip_class::heavy:      cond.precipitation = 1.0f;  break;
            case precip_class::none:
            case precip_class::last:
            default:                       cond.precipitation = 0.0f;  break;
        }
        // Saturating: the point is that being hurt is visible, not that the
        // effect keeps growing until the screen is unreadable.
        // Where the sun is, rather than where a renderer guessed. The 3D backend aims a
        // directional light with this; before it existed the bearing was a constant in
        // map_view_3d.gd, which is the kind of invention ADR-006 argued against on the
        // very page that then committed it.
        const std::pair<units::angle, units::angle> sun = sun_azimuth_altitude( calendar::turn );
        cond.sun_azimuth = static_cast<float>( to_degrees( sun.first ) );
        cond.sun_altitude = static_cast<float>( to_degrees( sun.second ) );
        cond.pain = std::clamp( static_cast<float>( u.get_perceived_pain() ) / 60.0f,
                                0.0f, 1.0f );
        lights.set_conditions( cond );
    }

    /**
     * Publish one tile of one z-level, and say whether the walk may carry on
     * downward past it (ADR-005 item 1).
     *
     * @param z_below levels below the one the avatar stands on; 0 is that level.
     *        Everything that differs between the floor you are on and a floor you
     *        are looking down at is decided from this one number.
     * @return false when there would be nothing to see below this tile anyway.
     */
    auto emit_column_tile = [&]( const tripoint_bub_ms & p, const int tx, const int ty,
    const int z_below ) -> bool {
        const bool top = z_below == 0;
        // Brightness for a lower level cannot come from the light pass: that
        // texture holds one texel per *column*, and the column's texel describes
        // the tile the avatar is looking through, not the one at the bottom of
        // the hole. Lower levels keep the CPU lighting the renderer used before
        // the pass existed, and MapView takes them out of the pass
        // (receives_light = false) rather than lighting them a second time with
        // another tile's light.
        const bool tile_light_pass = light_pass && top;

        const unsigned int seed = simple_point_hash( here.get_abs( p ).raw().xy() );

        if( !u.sees( here, p, true ) )
        {
            // Not currently visible: draw what the character remembers rather
            // than leaving a hole. Previously the whole tile was skipped, so
            // anything out of sight simply vanished.
            const memorized_tile &mem = u.get_memorized_tile( here.get_abs( p ) );
            const light_tints mem_tint = fog_for_depth(
                                             tints_for_light( lit_level::MEMORIZED, false, tile_light_pass ), z_below );
            // Remembered, not seen -- but only where there is actually a
            // memory. Marking every unseen tile as remembered would tell the
            // shader that the whole unexplored map is somewhere the player
            // has been, and LightSnapshot::begin has already cleared this to
            // "never seen".
            const bool remembered = !mem.get_ter_id().empty() || !mem.get_dec_id().empty();
            if( remembered ) {
                lights.set( tx, ty, z_below, LightSnapshot::vis_remembered, 0 );
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
            // A tile with no memory and no sight is the one case worth stopping
            // for: there is no way to have learned what is under a square you
            // have never seen. Where there *is* a memory, the floor check below
            // decides, as it does for a lit tile -- the floor cache is map data
            // and does not depend on being looked at.
            return remembered;
        }

        const lit_level ll = here.light_at( p );
        lights.set( tx, ty, z_below, LightSnapshot::vis_seen,
                    encode_light_level( ll, here.ambient_light_at( p ) ) );
        if( top )
        {
            // Light *sources*, for the 3D backend's real lights (ADR-006 item
            // 3D-2). The texture above says how lit each tile is, which is the
            // authority and stays so; this says where the light is coming from,
            // which a per-tile value cannot express and a 3D renderer needs.
            //
            // Read rather than derived: generate_lightmap filled this buffer this
            // turn, from terrain / furniture / field light_emitted, with each
            // source's colour. The estimate for this item assumed discrete lights
            // would have to be recovered from the lightmap; its inputs were one
            // struct away.
            //
            // Top level and seen only, matching the texture. A lamp you cannot see
            // is a lamp the game has already decided is not lighting you, and a
            // lamp in the basement would light the floor above it.
            //
            // **This reads state level_cache.h calls "only valid during
            // generate_lightmap".** It is scratch space: filled at the top of that
            // function and consumed inside it, never cleared at the end -- so after
            // do_turn it still holds this turn's sources, which is what this wants
            // and is not what it promises. The alternative is to re-derive the
            // sources here from terrain / furniture / field `light_emitted`, which
            // duplicates a rule the game owns and would drift from it silently.
            // Reading the game's own answer and saying so is the better trade, but
            // it is a trade: if this ever comes back empty, that contract is where
            // to look, and `lights` in the render stats is the number that says so.
            const auto &src = here.get_cache_ref( p.z() ).light_source_buffer[p.x()][p.y()];
            if( src.luminance > 0.0f ) {
                lights.add_light(
                    static_cast<float>( tx * tw ) + tw * 0.5f,
                    static_cast<float>( ty * th ) + th * 0.5f,
                    static_cast<float>( LIGHT_RANGE( src.luminance ) * tw ),
                    src.color, src.luminance );
            }
        }
        const light_tints tint = fog_for_depth(
                                     tints_for_light( ll, nv_goggles, tile_light_pass ), z_below );

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
            //
            // Top level only. Both channels here are 2D and per column: the
            // particle system positions emitters in the view's world space,
            // where there is no depth to put a lower one at, and the fire
            // channel of the light texture is one texel per column. A fire in
            // the basement would light the ground floor and drift its smoke
            // across it.
            const field_type &ftype = displayed.obj();
            if( top && ( ftype.has_fire || ftype.phase == phase_id::GAS ) ) {
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
                    lights.set_fire( tx, ty, z_below, static_cast<uint8_t>(
                                         std::min( 255, 90 + 55 * fc.intensity ) ) );
                }
            }
        }

        // Vehicles. vpart_display already resolves which part of a stack is
        // the visible one, including its tileset id.
        if( const optional_vpart_position ovp = here.veh_at( p ) ) {
            const vehicle &veh = ovp->vehicle();
            const vpart_display vd = veh.get_display_of_tile( ovp->mount_pos() );
            if( !vd.id.is_null() ) {
                // NOT get_tileset_id(). That joins the part and its variant
                // with vehicles::variant_separator, which is '#', and no
                // tileset id contains one -- Ultica has 656 vp_ ids and not a
                // single '#'. So every part that has a variant missed and
                // drew a placeholder, while the variantless ones (a trunk, a
                // siren) resolved and looked fine. That is why a car came out
                // as grey boxes with the occasional real part in it.
                //
                // '#' is the *memory* encoding; cata_tiles draws with the id
                // and the variant kept apart, which is what resolve_sprite
                // now takes.
                std::string vp_id = "vp_" + vd.id.str();

                // Open and broken are subtiles, as they are for terrain.
                const int sub = vd.is_open ? open_ : vd.is_broken ? broken : 0;
                if( sub != 0 ) {
                    if( const char *name = subtile_name( sub ) ) {
                        const std::string with_sub = vp_id + "_" + name;
                        if( find_tile_by_id_exact( with_sub ) ) {
                            vp_id = with_sub;
                        }
                    }
                }

                // The whole vehicle turns. Without this every part drew in
                // its default orientation, so the pieces never assembled into
                // a car no matter which sprites resolved.
                const int rot = angle_to_dir4( veh.face.dir() - 270_degrees );

                emit_tile_id( cmds, vp_id, sprite_category::vehicle_part, tx, ty,
                              map_layer::vehicle, false,
                              tint, simple_point_hash( ovp->mount_pos().raw() ),
                              rot, false, false, vd.variant.id );
            }
        }

        // Draw the whole visible stack, not just the first item: a pile of
        // loot rendered as one sprite hid everything under it.
        map_stack items = here.i_at( p );
        for( const item &it : items ) {
            emit_tile_id( cmds, it.typeId().str(), sprite_category::item, tx, ty, map_layer::item,
                          false, tint, seed );
        }
        return true;
    };

    /**
     * Walk each column of the view downward from the avatar's level.
     *
     * This is ADR-005 item 1, and the ADR budgeted it as the expensive one on the
     * grounds that per-tile work multiplies by the number of levels drawn. It
     * does not, because of the stop condition: `dont_draw_lower_floor` reads one
     * bool out of the level cache, and every tile with a floor under it -- which
     * on an outdoor level is every tile -- stops the walk after the level the
     * avatar is on. What descends is holes: a stairwell, a pit, the lip of a
     * roof, the shaft of a manhole. `open_columns` in the render stats is that
     * count, measured per frame rather than assumed, because the ADR's own
     * post-mortem is a list of features settled by printing a value.
     *
     * Deepest level per column is kept because the creature pass below needs it:
     * a zombie two floors down is only visible through the same hole its floor
     * is, and there is no cheaper way to ask that afterwards than to remember
     * where the walk stopped.
     */
    const int min_z = std::max( center.z() - fov_3d_z_range, -OVERMAP_DEPTH );
    // Lowest level published per column, indexed ty * view_w + tx.
    std::vector<int> column_floor( static_cast<size_t>( view_w ) * view_h, center.z() );
    int32_t open_columns = 0;

    for( int ty = 0; ty < view_h; ++ty ) {
        for( int tx = 0; tx < view_w; ++tx ) {
            for( int z = center.z(); z >= min_z && center.z() - z <= max_z_below; --z ) {
                const tripoint_bub_ms p( origin_x + tx, origin_y + ty, z );
                if( !here.inbounds( p ) ) {
                    break;
                }
                const int z_below = center.z() - z;
                // Read by emit_sprite, which is where the bits reach the command.
                // Threading it through emit_tile_id instead would mean touching
                // every call site for a value that is constant across a whole
                // level -- and getting one of them wrong is a sprite drawn at the
                // wrong depth, which looks like a sorting bug rather than a
                // missing argument.
                cur_z_below = z_below;
                const bool descend = emit_column_tile( p, tx, ty, z_below );
                column_floor[static_cast<size_t>( ty ) * view_w + tx] = z;
                if( !descend || here.dont_draw_lower_floor( p ) ) {
                    break;
                }
                // Counted here rather than off the floor cache alone: this is
                // where the walk actually went down, which is the thing the
                // number is for.
                if( z_below == 0 ) {
                    ++open_columns;
                }
            }
            cur_z_below = 0;
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
    std::vector<creature_record> creatures;
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
        // Which level it is standing on, and whether that level is one this
        // column actually published (ADR-005 item 1).
        //
        // This pass walks the creature list rather than the view, so it never
        // consulted z at all: a zombie in the basement was drawn on the ground
        // floor, at the right x and y, indistinguishable from one in the room.
        // u.sees() is true through a hole and stays true for fov_3d_z_range
        // levels, so this could not be left to it. A creature above the avatar
        // is dropped outright -- nothing above is published -- and one below is
        // drawn only as deep as the floor the column walk reached.
        const int z_below = center.z() - p.z();
        if( z_below < 0 || z_below > max_z_below ) {
            continue;
        }
        if( p.z() < column_floor[static_cast<size_t>( ty ) * view_w + tx] ) {
            continue;
        }
        cur_z_below = z_below;
        // What this creature is carrying or emitting (ADR-006 item 3D-2).
        //
        // Not in level_cache's buffered set: generate_lightmap applies a critter's glow
        // and a character's held light with apply_light_source, which writes the lightmap
        // and never goes through the buffer the light channel reads. So an NPC's torch and
        // a glowing zombie were invisible to it. Tapped here because this loop already has
        // the creature, its view coordinates, its visibility and its level in hand.
        //
        // The avatar's own is emitted after this loop; it is skipped at the top of it.
        if( z_below == 0 ) {
            float carried = 0.0f;
            if( const monster *glow = dynamic_cast<const monster *>( &critter ) ) {
                // mtype::luminance, without the enchantment modifier generate_lightmap
                // applies. A mutant's brighter glow is a difference of degree in a value
                // that is already only deciding how a light looks.
                carried = glow->type->luminance;
            } else if( const Character *lamp = critter.as_character() ) {
                carried = lamp->active_light();
            }
            if( carried > 0.0f ) {
                lights.add_light( static_cast<float>( tx * tw ) + tw * 0.5f,
                                  static_cast<float>( ty * th ) + th * 0.5f,
                                  static_cast<float>( LIGHT_RANGE( carried ) * tw ),
                                  light_color_rgb{}, carried );
            }
        }
        const light_tints critter_tint = fog_for_depth(
                                             tints_for_light( here.light_at( p ), nv_goggles,
                                                     light_pass ), z_below );
        // Identity, for the mesh path (3D-7c). Emitted for every visible creature whether
        // or not any art exists for it: a registry with nothing in it means every creature
        // falls back to its sprite, which is the current behaviour exactly.
        {
            creature_record rec;
            rec.x = tx * tw + tw / 2;
            rec.y = ( ty + 1 ) * th;
            rec.z_below = z_below;
            rec.flip = critter.as_character() != nullptr
                       ? critter.as_character()->facing == FacingDirection::LEFT
                       : false;
            if( const monster *mon = dynamic_cast<const monster *>( &critter ) ) {
                rec.id = mon->type->id.str();
                rec.kind = 0;
                rec.flip = mon->facing == FacingDirection::LEFT;
            } else if( const Character *ch = critter.as_character() ) {
                // The body sprite's id, not the character's name: it is what the tileset
                // keys on, so it is what a mesh registry should key on too.
                rec.id = ch->male ? "npc_male" : "npc_female";
                rec.kind = 1;
            }
            if( !rec.id.empty() ) {
                creatures.push_back( rec );
            }
        }
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
    // After the loop, not at the end of its body: half the paths through that
    // body are `continue`, so a reset inside it is skipped by exactly the
    // iterations that end early -- and the avatar, which is emitted next and is
    // always on the avatar's own level, would inherit the level of the last
    // creature that was culled.
    cur_z_below = 0;

    {
        const int tx = center.x() - origin_x;
        const int ty = center.y() - origin_y;
        // The avatar is always fully lit: it is the viewpoint, and dimming it
        // makes it hard to find on screen in the dark.
        const light_tints lit{ pack_tint( 255, 255, 255, 255 ),
                               pack_tint( 255, 255, 255, 255 ) };
        {
            creature_record rec;
            rec.id = u.male ? "player_male" : "player_female";
            rec.kind = 2;
            rec.x = tx * tw + tw / 2;
            rec.y = ( ty + 1 ) * th;
            rec.flip = u.facing == FacingDirection::LEFT;
            creatures.push_back( rec );
        }
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
    //
    // Depth is the first key now. MapView derives its own draw order from the
    // command's flags and does not depend on this ordering, but it does use "who
    // came first in the list" to break ties between batches at the same rank --
    // so the list has to agree with the ranking about which level is further
    // away, or two batches on different levels could tie-break each other's way.
    std::stable_sort( cmds.begin(), cmds.end(),
    []( const map_draw_cmd & a, const map_draw_cmd & b ) {
        const int32_t az = ( a.rot_flags & cmd_z_below_mask ) >> cmd_z_below_shift;
        const int32_t bz = ( b.rot_flags & cmd_z_below_mask ) >> cmd_z_below_shift;
        if( az != bz ) {
            // Deepest first: it is furthest from the camera.
            return az > bz;
        }
        if( a.layer != b.layer ) {
            return a.layer < b.layer;
        }
        if( a.dest_y != b.dest_y ) {
            return a.dest_y < b.dest_y;
        }
        return a.dest_x < b.dest_x;
    } );

    // What the z walk actually produced, counted off the finished list rather
    // than accumulated as it went: the creature pass emits below-level sprites
    // too, and a counter maintained in two places is a counter that disagrees
    // with itself.
    int32_t below_cmds = 0;
    int32_t deepest_z_below = 0;
    for( const map_draw_cmd &cmd : cmds ) {
        const int32_t z_below = ( cmd.rot_flags & cmd_z_below_mask ) >> cmd_z_below_shift;
        if( z_below > 0 ) {
            ++below_cmds;
            deepest_z_below = std::max( deepest_z_below, z_below );
        }
    }

    // The avatar's own light, and every headlight in view (ADR-006 item 3D-2). Both
    // bypass level_cache's buffered set -- one through apply_light_source, the other
    // through apply_light_arc -- so both are tapped rather than read.
    {
        const float held = u.active_light();
        if( held > 0.0f ) {
            const int tx = center.x() - origin_x;
            const int ty = center.y() - origin_y;
            if( tx >= 0 && ty >= 0 && tx < view_w && ty < view_h ) {
                lights.add_light( static_cast<float>( tx * tw ) + tw * 0.5f,
                                  static_cast<float>( ty * th ) + th * 0.5f,
                                  static_cast<float>( LIGHT_RANGE( held ) * tw ),
                                  light_color_rgb{}, held );
            }
        }
    }
    // Vehicle headlights, as beams rather than as blobs.
    //
    // This mirrors the vehicle loop in map::generate_lightmap, including its two passes:
    // the first sums what the cone lights on one vehicle add up to, with each further
    // lamp counting for a little less, and the second places them. Mirrored rather than
    // read because there is nothing to read -- an arc goes straight into the lightmap and
    // is never buffered -- which makes this the one duplicated rule in the channel. If
    // headlights ever stop agreeing with what the map says is lit, start here.
    for( wrapped_vehicle &wrapped : here.get_vehicles() ) {
        vehicle *veh = wrapped.v;
        if( veh == nullptr ) {
            continue;
        }
        const std::vector<vehicle_part *> lamps = veh->lights();
        float cone_luminance = 0.0f;
        float iteration = 1.0f;
        for( const vehicle_part *pt : lamps ) {
            const vpart_info &vp = pt->info();
            if( vp.has_flag( VPFLAG_CONE_LIGHT ) || vp.has_flag( VPFLAG_WIDE_CONE_LIGHT ) ) {
                cone_luminance += vp.bonus / iteration;
                iteration *= 1.1f;
            }
        }
        if( cone_luminance <= static_cast<float>( lit_level::LIT ) ) {
            continue;
        }
        for( const vehicle_part *pt : lamps ) {
            const vpart_info &vp = pt->info();
            const bool wide = vp.has_flag( VPFLAG_WIDE_CONE_LIGHT );
            if( !wide && !vp.has_flag( VPFLAG_CONE_LIGHT ) ) {
                continue;
            }
            const tripoint_bub_ms src = veh->bub_part_pos( here, *pt );
            const int tx = src.x() - origin_x;
            const int ty = src.y() - origin_y;
            if( tx < 0 || ty < 0 || tx >= view_w || ty >= view_h || src.z() != center.z() ) {
                continue;
            }
            if( !u.sees( here, src ) ) {
                continue;
            }
            lights.add_light( static_cast<float>( tx * tw ) + tw * 0.5f,
                              static_cast<float>( ty * th ) + th * 0.5f,
                              static_cast<float>( LIGHT_RANGE( cone_luminance ) * tw ),
                              vp.light_color, cone_luminance,
                              static_cast<float>( to_degrees( veh->face.dir() + pt->direction ) ),
                              wide ? 90.0f : 45.0f );
        }
    }

    lights.blur_fire();
    lights.commit();

    std::lock_guard<std::mutex> lock( mutex_ );
    cmds_ = std::move( cmds );
    glyphs_ = std::move( glyphs );
    fields_ = std::move( fields );
    avatar_overlays_ = std::move( avatar_overlays );
    creatures_ = std::move( creatures );
    fallback_counts_ = fb_counts;
    layer_counts_ = layer_counts;
    retracted_count_ = retracted;
    transparent_count_ = transparent;
    faded_count_ = faded;
    tall_candidates_ = tall_candidates;
    open_columns_ = open_columns;
    deepest_z_below_ = deepest_z_below;
    below_cmds_ = below_cmds;
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
