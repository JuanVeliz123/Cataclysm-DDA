#include <algorithm>
#include <filesystem>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cata_catch.h"
#include "cata_path.h"
#include "coordinates.h"
#include "map.h"
#include "map_helpers.h"
#include "mapbuffer.h"
#include "omdata.h"
#include "overmapbuffer.h"
#include "path_info.h"
#include "point.h"
#include "std_hash_fs_path.h"
#include "string_formatter.h"
#include "submap.h"
#include "type_id.h"
#include "worldfactory.h"
#include "zzip.h"

static const ter_str_id ter_t_floor( "t_floor" );
static const ter_str_id ter_t_wall( "t_wall" );

namespace
{

// Where a save file lives: a plain file, or an entry in a zzip.
struct stored_file {
    std::filesystem::path file;
    std::optional<std::filesystem::path> zzip_entry;
    std::filesystem::path dictionary;

    bool exists() const {
        if( !zzip_entry ) {
            return std::filesystem::exists( file );
        }
        std::optional<zzip> z = zzip::load( file, dictionary );
        return z && z->has_file( *zzip_entry );
    }

    // Removes the stored copy behind the back of whatever saved it, so that whether
    // the next save writes it again can be observed.
    void remove() const {
        if( !zzip_entry ) {
            std::filesystem::remove( file );
            return;
        }
        std::optional<zzip> z = zzip::load( file, dictionary );
        REQUIRE( z );
        REQUIRE( z->delete_files( { *zzip_entry } ) );
    }
};

bool compressed()
{
    return world_generator->active_world->has_compression_enabled();
}

stored_file quad_file( const tripoint_abs_omt &quad )
{
    const tripoint_abs_seg seg = project_to<coords::seg>( quad );
    const cata_path dir = PATH_INFO::current_dimension_save_path() / "maps" /
                          string_format( "%d.%d.%d", seg.x(), seg.y(), seg.z() );
    const std::filesystem::path name = std::filesystem::u8path(
                                           string_format( "%d.%d.%d.map", quad.x(), quad.y(), quad.z() ) );
    if( compressed() ) {
        return { ( dir + zzip_suffix ).get_unrelative_path(), name,
                 ( PATH_INFO::world_base_save_path() / "maps.dict" ).get_unrelative_path() };
    }
    return { ( dir / name ).get_unrelative_path(), std::nullopt, {} };
}

stored_file overmap_terrain_file( const point_abs_om &om )
{
    const std::filesystem::path name = std::filesystem::u8path( overmapbuffer::terrain_filename( om ) );
    if( compressed() ) {
        return { ( PATH_INFO::current_dimension_save_path() / zzip_overmap_directory / name +
                   zzip_suffix ).get_unrelative_path(), name,
                 ( PATH_INFO::world_base_save_path() / "overmaps.dict" ).get_unrelative_path() };
    }
    return { ( PATH_INFO::current_dimension_save_path() / name ).get_unrelative_path(), std::nullopt, {} };
}

stored_file overmap_view_file( const point_abs_om &om )
{
    return { overmapbuffer::player_filename( om ).get_unrelative_path(), std::nullopt, {} };
}

bool is_loaded( const tripoint_abs_sm &p )
{
    return std::any_of( MAPBUFFER.begin(), MAPBUFFER.end(), [&]( const auto & elem ) {
        return elem.first == p;
    } );
}

} // namespace

TEST_CASE( "mapbuffer_rewrites_only_changed_quads", "[map][mapbuffer][save]" )
{
    clear_map();
    const point_rel_sm quad_offsets[] = {
        point_rel_sm::zero, point_rel_sm::south, point_rel_sm::east, point_rel_sm::south_east
    };
    // A quad outside the reality bubble, so saving also unloads it, and not loaded yet.
    tripoint_abs_omt quad = project_to<coords::omt>( get_map().get_abs_sub() );
    const auto quad_is_free = [&]() {
        return !get_map().inbounds( quad ) &&
        std::none_of( std::begin( quad_offsets ), std::end( quad_offsets ), [&]( const point_rel_sm & o ) {
            return is_loaded( project_to<coords::sm>( quad ) + o.raw() );
        } );
    };
    do {
        quad += tripoint_rel_omt( 20, 0, 0 );
    } while( !quad_is_free() );
    const tripoint_abs_sm sm_addr = project_to<coords::sm>( quad );

    for( const point_rel_sm &offset : quad_offsets ) {
        std::unique_ptr<submap> sm = std::make_unique<submap>();
        sm->set_all_ter( ter_t_floor.id() );
        REQUIRE( MAPBUFFER.add_submap( sm_addr + offset.raw(), sm ) );
    }
    MAPBUFFER.lookup_submap( sm_addr )->set_ter( point_sm_ms( 1, 1 ), ter_t_wall.id() );

    MAPBUFFER.save();
    const stored_file stored = quad_file( quad );
    REQUIRE( stored.exists() );
    CHECK_FALSE( is_loaded( sm_addr ) );

    // Loaded back as it was saved.
    submap *sm = MAPBUFFER.lookup_submap( sm_addr );
    REQUIRE( sm != nullptr );
    CHECK( sm->get_ter( point_sm_ms( 1, 1 ) ) == ter_t_wall.id() );
    CHECK( sm->get_ter( point_sm_ms( 2, 2 ) ) == ter_t_floor.id() );

    SECTION( "an unchanged quad is not written again" ) {
        stored.remove();
        MAPBUFFER.save();
        CHECK_FALSE( is_loaded( sm_addr ) );
        CHECK_FALSE( stored.exists() );
    }

    SECTION( "a changed quad is written again" ) {
        stored.remove();
        sm->set_ter( point_sm_ms( 2, 2 ), ter_t_wall.id() );
        MAPBUFFER.save();
        CHECK_FALSE( is_loaded( sm_addr ) );
        REQUIRE( stored.exists() );

        sm = MAPBUFFER.lookup_submap( sm_addr );
        REQUIRE( sm != nullptr );
        CHECK( sm->get_ter( point_sm_ms( 1, 1 ) ) == ter_t_wall.id() );
        CHECK( sm->get_ter( point_sm_ms( 2, 2 ) ) == ter_t_wall.id() );
    }

    SECTION( "a quad reloaded after an unchanged save and then changed is written again" ) {
        MAPBUFFER.save();
        sm = MAPBUFFER.lookup_submap( sm_addr );
        REQUIRE( sm != nullptr );
        sm->set_ter( point_sm_ms( 3, 3 ), ter_t_wall.id() );
        MAPBUFFER.save();

        sm = MAPBUFFER.lookup_submap( sm_addr );
        REQUIRE( sm != nullptr );
        CHECK( sm->get_ter( point_sm_ms( 1, 1 ) ) == ter_t_wall.id() );
        CHECK( sm->get_ter( point_sm_ms( 3, 3 ) ) == ter_t_wall.id() );
    }
}

TEST_CASE( "overmapbuffer_rewrites_only_changed_overmaps", "[overmap][save]" )
{
    clear_overmaps();
    const point_abs_om om = point_abs_om::zero;
    const tripoint_abs_omt p = project_combine( om, tripoint_om_omt( 10, 10, 0 ) );
    overmap_buffer.get( om );

    overmap_buffer.save();
    const stored_file terrain = overmap_terrain_file( om );
    const stored_file view = overmap_view_file( om );
    REQUIRE( terrain.exists() );
    REQUIRE( view.exists() );
    terrain.remove();
    view.remove();

    SECTION( "an unchanged overmap is not written again" ) {
        overmap_buffer.save();
        CHECK_FALSE( terrain.exists() );
        CHECK_FALSE( view.exists() );
    }

    SECTION( "a changed view is written again" ) {
        overmap_buffer.add_note( p, "a note to change the view" );
        overmap_buffer.save();
        CHECK_FALSE( terrain.exists() );
        CHECK( view.exists() );
    }

    SECTION( "changed terrain is written again" ) {
        const oter_id forest( "forest" );
        const oter_id field( "field" );
        const oter_id changed = overmap_buffer.ter( p ) == forest ? field : forest;
        overmap_buffer.ter_set( p, changed );
        overmap_buffer.add_note( p, "a note to change the view" );
        overmap_buffer.save();
        REQUIRE( terrain.exists() );
        REQUIRE( view.exists() );

        clear_overmaps();
        CHECK( overmap_buffer.ter( p ) == changed );
        CHECK( overmap_buffer.note( p ) == "a note to change the view" );
    }
}

TEST_CASE( "mapbuffer_compacts_far_quads_without_writing_them", "[map][mapbuffer][save]" )
{
    clear_map();
    // Catch runs this again for each section; each run gets its own, untouched area.
    MAPBUFFER.clear_outside_reality_bubble();
    static int run = 0;
    const point_rel_sm quad_offsets[] = {
        point_rel_sm::zero, point_rel_sm::south, point_rel_sm::east, point_rel_sm::south_east
    };
    // Enough far quads that compacting is worth it.
    const tripoint_abs_omt origin = project_to<coords::omt>( get_map().get_abs_sub() ) +
                                    tripoint_rel_omt( 100, 100 + 30 * run++, 0 );
    std::vector<tripoint_abs_omt> quads;
    for( int x = 0; x < 20; ++x ) {
        for( int y = 0; y < 20; ++y ) {
            quads.push_back( origin + tripoint_rel_omt( x, y, 0 ) );
        }
    }
    for( const tripoint_abs_omt &quad : quads ) {
        REQUIRE_FALSE( get_map().inbounds( quad ) );
        for( const point_rel_sm &offset : quad_offsets ) {
            std::unique_ptr<submap> sm = std::make_unique<submap>();
            sm->set_all_ter( ter_t_floor.id() );
            REQUIRE( MAPBUFFER.add_submap( project_to<coords::sm>( quad ) + offset.raw(), sm ) );
        }
    }
    const tripoint_abs_omt quad = quads.front();
    const tripoint_abs_sm sm_addr = project_to<coords::sm>( quad );
    MAPBUFFER.lookup_submap( sm_addr )->set_ter( point_sm_ms( 1, 1 ), ter_t_wall.id() );

    for( int i = 0; i < 20 && is_loaded( sm_addr ); ++i ) {
        MAPBUFFER.compact_far_quads();
    }
    REQUIRE_FALSE( is_loaded( sm_addr ) );
    // Nothing reaches the disk before a save, so a crash can't leave it half saved.
    const stored_file stored = quad_file( quad );
    CHECK_FALSE( stored.exists() );

    SECTION( "a compacted quad comes back as it was" ) {
        submap *sm = MAPBUFFER.lookup_submap( sm_addr );
        REQUIRE( sm != nullptr );
        CHECK( sm->get_ter( point_sm_ms( 1, 1 ) ) == ter_t_wall.id() );
        CHECK( sm->get_ter( point_sm_ms( 2, 2 ) ) == ter_t_floor.id() );
        CHECK( MAPBUFFER.submap_exists( sm_addr + point_rel_sm::south_east.raw() ) );
    }

    SECTION( "a compacted quad is saved" ) {
        MAPBUFFER.save();
        REQUIRE( stored.exists() );
        submap *sm = MAPBUFFER.lookup_submap( sm_addr );
        REQUIRE( sm != nullptr );
        CHECK( sm->get_ter( point_sm_ms( 1, 1 ) ) == ter_t_wall.id() );
    }

    SECTION( "a quad changed after coming back from compaction is saved changed" ) {
        submap *sm = MAPBUFFER.lookup_submap( sm_addr );
        REQUIRE( sm != nullptr );
        sm->set_ter( point_sm_ms( 2, 2 ), ter_t_wall.id() );
        for( int i = 0; i < 20 && is_loaded( sm_addr ); ++i ) {
            MAPBUFFER.compact_far_quads();
        }
        MAPBUFFER.save();
        sm = MAPBUFFER.lookup_submap( sm_addr );
        REQUIRE( sm != nullptr );
        CHECK( sm->get_ter( point_sm_ms( 1, 1 ) ) == ter_t_wall.id() );
        CHECK( sm->get_ter( point_sm_ms( 2, 2 ) ) == ter_t_wall.id() );
    }
}
