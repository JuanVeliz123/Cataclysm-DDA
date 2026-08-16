#include "godot_overmap_snapshot.h"

#if defined(GODOT)

#include "avatar.h"
#include "cata_utility.h"
#include "coordinates.h"
#include "game.h"
#include "godot_tileset_loader.h"
#include "map_extras.h"
#include "enums.h"
#include "omdata.h"
#include "options.h"
#include "output.h"
#include "overmap.h"
#include "overmapbuffer.h"
#include "type_id.h"
#include "uistate.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_map>

#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot_backend
{

namespace
{

OvermapSnapshot g_overmap_snapshot;
godot_tileset g_om_tileset;
/// Texture pointer -> exported atlas index, filled at load.
std::unordered_map<const void *, int> g_om_atlas_index;

/// Resolve an overmap sprite id, following looks_like the way the tileset loader
/// intends, and falling back to "unknown_terrain" rather than nothing so a missing
/// sprite shows as a marker instead of a hole.
const godot_tile_type *find_om_tile( const std::string &id, int jumps = 8 )
{
    if( jumps <= 0 || id.empty() ) {
        return nullptr;
    }
    if( const godot_tile_type *t = g_om_tileset.find_tile_type( id ) ) {
        return t;
    }
    // oter_type_t carries a whole fallback chain rather than a single id, so try
    // each in turn.
    const oter_str_id oter_sid( id );
    if( oter_sid.is_valid() ) {
        for( const std::string &ll : oter_sid.obj().get_type_id()->looks_like ) {
            if( ll.empty() || ll == id ) {
                continue;
            }
            if( const godot_tile_type *t = find_om_tile( ll, jumps - 1 ) ) {
                return t;
            }
        }
    }
    return g_om_tileset.find_tile_type( "unknown_terrain" );
}

/// Neighbour mask -> (subtile, rotation), the same table godot_map_snapshot uses.
/// Overmap connections are built here in the order {south, east, west, north} to
/// match, which is also the order get_omt_id_rotation_and_subtile uses.
void om_connections_to_subtile( const uint8_t val, int &subtile, int &rotation )
{
    struct entry {
        MULTITILE_TYPE subtile;
        int rotation;
    };
    static constexpr std::array<entry, 16> table = { {
            { unconnected, 0 }, { end_piece, 0 }, { end_piece, 3 }, { corner, 0 },
            { end_piece, 1 }, { corner, 1 }, { edge, 1 }, { t_connection, 0 },
            { end_piece, 2 }, { edge, 0 }, { corner, 3 }, { t_connection, 3 },
            { corner, 2 }, { t_connection, 1 }, { t_connection, 2 }, { center, 0 },
        }
    };
    const entry &e = table[val & 0x0F];
    subtile = static_cast<int>( e.subtile );
    rotation = e.rotation;
}

const char *om_subtile_name( const int subtile )
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

/// The overmap terrain at @p p as the tileset should see it: blended where the
/// terrain blends at this vision level, and with forest trails folded into plain
/// forest when the player has that display option off. Mirrors the oter_at lambda
/// inside cata_tiles::get_omt_id_rotation_and_subtile.
oter_id om_oter_at( const tripoint_abs_omt &p )
{
    oter_id cur_ter = overmap_buffer.ter( p );
    const om_vision_level vision = overmap_buffer.seen( p );
    if( cur_ter->blends_adjacent( vision ) ) {
        cur_ter = oter_vision::get_blended_omt_info( p, vision ).id;
    }
    if( !uistate.overmap_show_forest_trails &&
        cur_ter->get_type_id() == oter_type_str_id( "forest_trail" ) ) {
        return oter_id( "forest" );
    }
    return cur_ter;
}

/// See pick_sprite_rota in godot_map_snapshot.cpp: a multi-entry sprite list holds
/// orientations the tileset drew itself, so it is indexed by the rotation rather
/// than rotated at draw time.
int pick_om_sprite( const weighted_int_list<std::vector<int>> &list, unsigned int seed,
                    int rota, bool &rotate_out )
{
    const std::vector<int> *picked = list.pick( seed );
    if( !picked || picked->empty() ) {
        rotate_out = false;
        return -1;
    }
    const std::vector<int> &sprites = *picked;
    if( sprites.size() == 1 ) {
        rotate_out = true;
        return sprites[0];
    }
    rotate_out = false;
    return sprites[static_cast<size_t>( std::max( 0, rota ) ) % sprites.size()];
}

} // namespace

bool OvermapSnapshot::ensure_tileset_loaded()
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        if( ready_ ) {
            return true;
        }
    }

    // CDDA keeps the overmap tileset in its own option; it is a different art
    // style and a different sprite-per-id mapping than the map tileset.
    std::string id = get_option<std::string>( "OVERMAP_TILES" );
    if( TILESETS.find( id ) == TILESETS.end() ) {
        if( TILESETS.count( "Larwick_Overmap" ) ) {
            id = "Larwick_Overmap";
        } else if( !TILESETS.empty() ) {
            id = TILESETS.begin()->first;
        } else {
            godot::UtilityFunctions::printerr( "OvermapSnapshot: no tilesets found under gfx/" );
            return false;
        }
    }

    try {
        godot_tileset_loader loader;
        loader.load( g_om_tileset, id, /*precheck=*/false, /*pump_events=*/false,
                     /*terrain=*/false );
    } catch( const std::exception &e ) {
        godot::UtilityFunctions::printerr( "OvermapSnapshot: tileset load failed: ", e.what() );
        std::lock_guard<std::mutex> lock( mutex_ );
        ready_ = false;
        return false;
    }

    std::vector<atlas_pixels> atlases;
    g_om_atlas_index.clear();
    for( const godot_texture &sprite : g_om_tileset.tile_values ) {
        if( !sprite.is_valid() ) {
            continue;
        }
        const godot::Ref<godot::ImageTexture> &tex = sprite.get_texture();
        const void *key = tex.ptr();
        if( g_om_atlas_index.count( key ) ) {
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
        g_om_atlas_index[key] = static_cast<int>( atlases.size() );
        atlases.push_back( std::move( ap ) );
    }

    {
        std::lock_guard<std::mutex> lock( mutex_ );
        tileset_id_ = id;
        tile_w_ = std::max( 1, g_om_tileset.get_tile_width() );
        tile_h_ = std::max( 1, g_om_tileset.get_tile_height() );
        atlases_ = std::move( atlases );
        ready_ = !atlases_.empty();
        cmds_.clear();
    }

    godot::UtilityFunctions::print(
        "OvermapSnapshot: loaded tileset ", id.c_str(),
        " atlases=", static_cast<int64_t>( g_om_atlas_index.size() ),
        " tile=", tile_width(), "x", tile_height() );
    return ready_;
}

bool OvermapSnapshot::tileset_ready() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return ready_;
}

std::string OvermapSnapshot::tileset_id() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return tileset_id_;
}

int OvermapSnapshot::tile_width() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return tile_w_;
}

int OvermapSnapshot::tile_height() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return tile_h_;
}

int OvermapSnapshot::atlas_count() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return static_cast<int>( atlases_.size() );
}

godot::Ref<godot::Image> OvermapSnapshot::copy_atlas_image( int index ) const
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

void OvermapSnapshot::release_resources()
{
    // g_om_tileset holds Ref<ImageTexture> for every overmap atlas; see
    // release_godot_resources() in godot_backend.h for why this cannot wait
    // for static destruction.
    g_om_tileset.clear();
}

void OvermapSnapshot::update_from_game( const tripoint_abs_omt &center,
                                        const tripoint_abs_omt &cursor, const bool blink )
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        if( !ready_ ) {
            return;
        }
    }
    if( !g ) {
        return;
    }

    // The overmap window is sized in terminal cells; one cell is one overmap
    // terrain, so the cell dimensions are the tile counts.
    int view_w = OVERMAP_WINDOW_WIDTH > 0 ? OVERMAP_WINDOW_WIDTH : 80;
    int view_h = OVERMAP_WINDOW_HEIGHT > 0 ? OVERMAP_WINDOW_HEIGHT : 24;
    view_w = std::clamp( view_w, 11, 180 );
    view_h = std::clamp( view_h, 11, 120 );

    const int origin_x = center.x() - view_w / 2;
    const int origin_y = center.y() - view_h / 2;
    const int tw = std::max( 1, g_om_tileset.get_tile_width() );
    const int th = std::max( 1, g_om_tileset.get_tile_height() );

    avatar &you = get_avatar();
    const tripoint_abs_omt avatar_pos = you.pos_abs_omt();

    std::vector<map_draw_cmd> cmds;
    cmds.reserve( static_cast<size_t>( view_w * view_h ) + 8 );

    auto emit = [&]( const std::string & id, int dest_tx, int dest_ty, overmap_layer layer,
                     unsigned int seed, int rotation = 0 ) {
        const godot_tile_type *tt = find_om_tile( id );
        if( !tt ) {
            return;
        }
        bool rotate_sprite = false;
        const int sprite_idx = pick_om_sprite( tt->fg, seed, rotation, rotate_sprite );
        if( sprite_idx < 0 || sprite_idx >= static_cast<int>( g_om_tileset.tile_values.size() ) ) {
            return;
        }
        const godot_texture &tex = g_om_tileset.tile_values[static_cast<size_t>( sprite_idx )];
        if( !tex.is_valid() ) {
            return;
        }
        const auto it = g_om_atlas_index.find( tex.get_texture().ptr() );
        if( it == g_om_atlas_index.end() ) {
            return;
        }
        map_draw_cmd cmd;
        cmd.atlas = it->second;
        cmd.src_x = tex.src_x();
        cmd.src_y = tex.src_y();
        cmd.src_w = tex.src_w();
        cmd.src_h = tex.src_h();
        cmd.dest_x = dest_tx * tw + tt->offset.x;
        cmd.dest_y = dest_ty * th + tt->offset.y;
        cmd.layer = static_cast<int32_t>( layer );
        // Explored-but-not-currently-visible overmap tiles are dimmed, which is
        // what the SDL path expresses by picking its grayscale atlas variant.
        cmd.tint = static_cast<int32_t>( 0xFFFFFFFF );
        cmd.rot_flags = rotate_sprite ? ( rotation & cmd_rotation_mask ) : 0;
        cmds.push_back( cmd );
    };

    for( int ty = 0; ty < view_h; ++ty ) {
        for( int tx = 0; tx < view_w; ++tx ) {
            const tripoint_abs_omt omp( origin_x + tx, origin_y + ty, center.z() );
            const unsigned int seed = static_cast<unsigned int>( omp.x() + omp.y() * 65536 );

            const om_vision_level vision = overmap_buffer.seen( omp );
            std::string id;
            int rotation = 0;
            if( vision == om_vision_level::unseen ) {
                id = "unknown_terrain";
            } else {
                // get_tileset_id prefixes real overmap terrains with "om#"; the
                // remainder is the sprite id, and anything without the prefix is a
                // vision-level placeholder rather than a terrain.
                const oter_id ot = om_oter_at( omp );
                std::string tileset_id = ot->get_tileset_id( vision );
                id = tileset_id.size() > 3 && tileset_id.compare( 0, 3, "om#" ) == 0
                     ? tileset_id.substr( 3 )
                     : tileset_id;

                // Roads, rivers, railways and walls need their connected subtile,
                // or the whole overmap reads as a field of disconnected stubs.
                const oter_type_id ot_type = ot->get_type_id();
                const bool connects = ot_type->has_connections();
                const bool is_water = ot_type->has_flag( oter_flags::water );
                if( connects || is_water ) {
                    const std::array<oter_type_id, 4> neighbours = {
                        om_oter_at( omp + point::south )->get_type_id(),
                        om_oter_at( omp + point::east )->get_type_id(),
                        om_oter_at( omp + point::west )->get_type_id(),
                        om_oter_at( omp + point::north )->get_type_id()
                    };
                    uint8_t val = 0;
                    for( int i = 0; i < 4; ++i ) {
                        const bool joined = connects
                                            ? ot_type->connects_to( neighbours[i] )
                                            : neighbours[i]->has_flag( oter_flags::water );
                        if( joined ) {
                            val |= static_cast<uint8_t>( 1 << i );
                        }
                    }
                    int subtile = 0;
                    om_connections_to_subtile( val, subtile, rotation );
                    if( const char *name = om_subtile_name( subtile ) ) {
                        const std::string candidate = id + "_" + name;
                        if( g_om_tileset.find_tile_type( candidate ) ) {
                            id = candidate;
                        } else {
                            rotation = 0;
                        }
                    }
                }
            }
            emit( id, tx, ty, overmap_layer::terrain, seed, rotation );

            if( vision == om_vision_level::unseen ) {
                continue;
            }

            const map_extra_id mx = overmap_buffer.extra( omp );
            if( !mx.is_empty() && mx->visibility != map_extra_visibility::none ) {
                emit( mx.str(), tx, ty, overmap_layer::map_extra, seed );
            }

            if( overmap_buffer.has_note( omp ) ) {
                emit( "note", tx, ty, overmap_layer::note, seed );
            }
        }
    }

    // The avatar, then the selection cursor on top of everything.
    if( avatar_pos.z() == center.z() ) {
        const int tx = avatar_pos.x() - origin_x;
        const int ty = avatar_pos.y() - origin_y;
        if( tx >= 0 && ty >= 0 && tx < view_w && ty < view_h ) {
            emit( "player_female", tx, ty, overmap_layer::player, 0 );
        }
    }
    if( blink && cursor.z() == center.z() ) {
        const int tx = cursor.x() - origin_x;
        const int ty = cursor.y() - origin_y;
        if( tx >= 0 && ty >= 0 && tx < view_w && ty < view_h ) {
            emit( "cursor", tx, ty, overmap_layer::cursor, 0 );
        }
    }

    std::sort( cmds.begin(), cmds.end(), []( const map_draw_cmd & a, const map_draw_cmd & b ) {
        if( a.layer != b.layer ) {
            return a.layer < b.layer;
        }
        if( a.dest_y != b.dest_y ) {
            return a.dest_y < b.dest_y;
        }
        return a.dest_x < b.dest_x;
    } );

    std::lock_guard<std::mutex> lock( mutex_ );
    cmds_ = std::move( cmds );
    view_w_ = view_w;
    view_h_ = view_h;
    origin_x_ = origin_x;
    origin_y_ = origin_y;
    ++generation_;
}

godot::PackedInt32Array OvermapSnapshot::copy_draw_list() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::PackedInt32Array out;
    out.resize( static_cast<int64_t>( cmds_.size() * MapSnapshot::cmd_stride ) );
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

godot::Vector2i OvermapSnapshot::view_size_tiles() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return godot::Vector2i( view_w_, view_h_ );
}

godot::Vector2i OvermapSnapshot::view_origin_tiles() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return godot::Vector2i( origin_x_, origin_y_ );
}

int OvermapSnapshot::command_count() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return static_cast<int>( cmds_.size() );
}

uint64_t OvermapSnapshot::generation() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return generation_;
}

bool OvermapSnapshot::active() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return active_;
}

void OvermapSnapshot::set_active( const bool active )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    active_ = active;
    if( !active ) {
        // Drop the draw list so a reopened overmap cannot flash the previous view
        // before the first redraw lands.
        cmds_.clear();
        ++generation_;
    }
}

OvermapSnapshot &get_overmap_snapshot()
{
    return g_overmap_snapshot;
}

overmap_active_guard::overmap_active_guard()
{
    get_overmap_snapshot().ensure_tileset_loaded();
    get_overmap_snapshot().set_active( true );
}

overmap_active_guard::~overmap_active_guard()
{
    get_overmap_snapshot().set_active( false );
}

void update_overmap_snapshot( const tripoint_abs_omt &center, const tripoint_abs_omt &cursor,
                              const bool blink )
{
    get_overmap_snapshot().update_from_game( center, cursor, blink );
}

} // namespace godot_backend

#endif // GODOT
