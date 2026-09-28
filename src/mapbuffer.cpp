#include "mapbuffer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cata_path.h"
#include "cata_utility.h"
#include "debug.h"
#include "filesystem.h"
#include "flexbuffer_json.h"
#include "game.h"
#include "input.h"
#include "json.h"
#include "json_loader.h"
#include "map.h"
#include "map_scale_constants.h"
#include "output.h"
#include "overmapbuffer.h"
#include "path_info.h"
#include "point.h"
#include "popup.h"
#include "std_hash_fs_path.h"
#include "string_formatter.h"
#include "submap.h"
#include "translations.h"
#include "type_id.h"
#include "ui_manager.h"
#include "worldfactory.h"
#include "zzip.h"

#include <zstd/common/xxhash.h>
#include <zstd/zstd.h>

#define dbg(x) DebugLog((x),D_MAP) << __FILE__ << ":" << __LINE__ << ": "

static std::string quad_file_name( const tripoint_abs_omt &om_addr )
{
    return string_format( "%d.%d.%d.map", om_addr.x(), om_addr.y(), om_addr.z() );
}

// A segment is a chunk of 32x32 submap quads.
// We're breaking them into subdirectories so there aren't too many files per directory.
static cata_path segment_dirname( const tripoint_abs_seg &segment_addr )
{
    std::string segment = string_format( "%d.%d.%d",
                                         segment_addr.x(),
                                         segment_addr.y(), segment_addr.z() );
    return PATH_INFO::current_dimension_save_path() / "maps" / segment;
}

static cata_path find_dirname( const tripoint_abs_omt &om_addr )
{
    return segment_dirname( project_to<coords::seg>( om_addr ) );
}

static cata_path maps_dictionary_path()
{
    return PATH_INFO::world_base_save_path() / "maps.dict";
}

static uint64_t quad_content_hash( std::string_view content )
{
    return XXH64( content.data(), content.size(), 0 );
}

// Compaction trades a little CPU for a lot of memory, so it favours speed.
static std::string compress_quad( std::string_view content )
{
    static const std::unique_ptr<ZSTD_CCtx, size_t( * )( ZSTD_CCtx * )> cctx( ZSTD_createCCtx(),
            &ZSTD_freeCCtx );
    std::string compressed( ZSTD_compressBound( content.size() ), '\0' );
    const size_t size = ZSTD_compressCCtx( cctx.get(), compressed.data(), compressed.size(),
                                           content.data(), content.size(), 1 );
    if( ZSTD_isError( size ) ) {
        throw std::runtime_error( std::string( "Failed compressing a quad: " ) +
                                  ZSTD_getErrorName( size ) );
    }
    compressed.resize( size );
    compressed.shrink_to_fit();
    return compressed;
}

static std::string decompress_quad( std::string_view compressed, size_t size )
{
    std::string content( size, '\0' );
    const size_t result = ZSTD_decompress( content.data(), content.size(), compressed.data(),
                                           compressed.size() );
    if( ZSTD_isError( result ) || result != size ) {
        throw std::runtime_error( "Failed decompressing a quad" );
    }
    return content;
}

// Live submaps beyond the reality bubble tolerated before compact_far_quads() does anything.
static constexpr size_t far_submaps_before_compacting = 1024;
// Bounds the work done in one call, so crossing a lot of map at once doesn't stall a turn.
static constexpr size_t max_quads_compacted_per_call = 128;
// Writing a save is mostly compression; beyond a few threads the disk is the limit.
static constexpr size_t max_save_threads = 8;

mapbuffer MAPBUFFER;

mapbuffer::mapbuffer() = default;
mapbuffer::~mapbuffer() = default;

void mapbuffer::clear()
{
    submaps.clear();
    cold_quads.clear();
    quad_disk_hashes.clear();
}

void mapbuffer::clear_outside_reality_bubble()
{
    cold_quads.clear();
    map &here = get_map();
    auto it = submaps.begin();
    while( it != submaps.end() ) {
        if( here.inbounds( it->first ) ) {
            ++it;
        } else {
            it = submaps.erase( it );
        }
    }
}

bool mapbuffer::add_submap( const tripoint_abs_sm &p, std::unique_ptr<submap> &sm )
{
    // A compacted quad still counts as present.
    thaw_quad( p );
    if( submaps.count( p ) ) {
        return false;
    }

    submaps[p] = std::move( sm );

    return true;
}

bool mapbuffer::add_submap( const tripoint_abs_sm &p, submap *sm )
{
    // FIXME: get rid of this overload and make submap ownership semantics sane.
    std::unique_ptr<submap> temp( sm );
    bool result = add_submap( p, temp );
    if( !result ) {
        // NOLINTNEXTLINE( bugprone-unused-return-value )
        temp.release();
    }
    return result;
}

void mapbuffer::remove_submap( const tripoint_abs_sm &addr )
{
    auto m_target = submaps.find( addr );
    if( m_target == submaps.end() ) {
        debugmsg( "Tried to remove non-existing submap %s", addr.to_string() );
        return;
    }
    submaps.erase( m_target );
}

submap *mapbuffer::lookup_submap( const tripoint_abs_sm &p )
{
    dbg( D_INFO ) << "mapbuffer::lookup_submap( x[" << p.x() << "], y[" << p.y() << "], z["
                  << p.z() << "])";

    const auto iter = submaps.find( p );
    if( iter == submaps.end() ) {
        try {
            if( thaw_quad( p ) ) {
                const auto thawed = submaps.find( p );
                return thawed == submaps.end() ? nullptr : thawed->second.get();
            }
            return unserialize_submaps( p );
        } catch( const std::exception &err ) {
            debugmsg( "Failed to load submap %s: %s", p.to_string(), err.what() );
        }
        return nullptr;
    }

    return iter->second.get();
}

bool mapbuffer::submap_exists( const tripoint_abs_sm &p )
{
    // Could so with a second check against a std::unordered_set<tripoint_abs_sm> of already checked existing but not loaded submaps before resorting to unserializing?
    const auto iter = submaps.find( p );
    if( iter == submaps.end() ) {
        try {
            if( thaw_quad( p ) ) {
                return submaps.count( p ) > 0;
            }
            return unserialize_submaps( p );
        } catch( const std::exception &err ) {
            debugmsg( "Failed to load submap %s: %s", p.to_string(), err.what() );
        }
        return false;
    }

    return true;
}

bool mapbuffer::submap_exists_approx( const tripoint_abs_sm &p )
{
    const auto iter = submaps.find( p );
    if( iter == submaps.end() ) {
        try {
            const tripoint_abs_omt om_addr = project_to<coords::omt>( p );
            if( cold_quads.count( om_addr ) ) {
                return true;
            }
            const cata_path dirname = find_dirname( om_addr );
            std::string file_name = quad_file_name( om_addr );

            if( world_generator->active_world->has_compression_enabled() ) {
                cata_path zzip_name = dirname;
                zzip_name += zzip_suffix;
                if( !file_exist( zzip_name ) ) {
                    return false;
                }
                std::optional<zzip> z = zzip::load( zzip_name.get_unrelative_path(),
                                                    maps_dictionary_path().get_unrelative_path() );
                return z && z->has_file( std::filesystem::u8path( file_name ) );
            } else {
                return file_exist( dirname / file_name );
            }
        } catch( const std::exception &err ) {
            debugmsg( "Failed to load submap %s: %s", p.to_string(), err.what() );
        }
        return false;
    }

    return true;
}

namespace
{
// Everything save() writes into one segment, prepared on the game thread so that writing
// it touches no game state.
struct segment_write {
    cata_path dirname;
    std::vector<std::pair<std::filesystem::path, std::string>> files;
    // Written first, like every file, then removed: see "remove_file" in save().
    std::unordered_set<std::filesystem::path, std_fs_path_hash> removed;
    // Committed to quad_disk_hashes only once the segment is written.
    std::vector<std::pair<std::string, uint64_t>> written_hashes;
    std::vector<tripoint_abs_omt> saved_cold_quads;
};

void write_segment( const segment_write &segment, bool compressed,
                    const std::filesystem::path &dictionary )
{
    if( !compressed ) {
        if( !segment.files.empty() ) {
            // Don't create the directory if it would be empty
            assure_dir_exist( segment.dirname );
        }
        for( const std::pair<std::filesystem::path, std::string> &file : segment.files ) {
            write_to_file( segment.dirname / file.first, [&]( std::ostream & fout ) {
                fout << file.second;
            } );
        }
        for( const std::filesystem::path &file : segment.removed ) {
            std::filesystem::remove( ( segment.dirname / file ).get_unrelative_path() );
        }
        return;
    }

    cata_path zzip_name = segment.dirname;
    zzip_name += zzip_suffix;
    std::optional<zzip> z = zzip::load( zzip_name.get_unrelative_path(), dictionary );
    if( !z ) {
        throw std::runtime_error( "Failed opening compressed save file " +
                                  zzip_name.get_unrelative_path().generic_u8string() );
    }
    std::vector<std::pair<std::filesystem::path, std::string_view>> files;
    files.reserve( segment.files.size() );
    for( const std::pair<std::filesystem::path, std::string> &file : segment.files ) {
        files.emplace_back( file.first, file.second );
    }
    if( !z->add_files( files ) ) {
        throw std::runtime_error( "Failed writing compressed save file " +
                                  zzip_name.get_unrelative_path().generic_u8string() );
    }
    if( !segment.removed.empty() ) {
        z->delete_files( segment.removed );
    }
    cata_path tmp_path = zzip_name + ".tmp";
    if( z->compact_to( tmp_path.get_unrelative_path(), 2.0 ) ) {
        z.reset();
        rename_file( tmp_path, zzip_name );
    }
}
} // namespace

void mapbuffer::save( bool delete_after_save )
{
    assure_dir_exist( PATH_INFO::current_dimension_save_path() / "maps" );
    int num_saved_submaps = 0;
    int num_total_submaps = submaps.size() + 4 * cold_quads.size();

    map &here = get_map();
    const bool compressed = world_generator->active_world->has_compression_enabled();

    static_popup popup;

    // Whatever the coordinates of the current submap are,
    // we're saving a 2x2 quad of submaps at a time.
    // Submaps are generated in quads, so we know if we have one member of a quad,
    // we have the rest of it, if that assumption is broken we have REAL problems.
    // The quads are grouped by the segment that stores them, so each segment's file is
    // opened, written and compacted once per save instead of once per quad.
    std::map<tripoint_abs_seg, std::vector<tripoint_abs_omt>> quads_by_segment;
    {
        std::set<tripoint_abs_omt> seen_quads;
        for( auto &elem : submaps ) {
            const tripoint_abs_omt om_addr = project_to<coords::omt>( elem.first );
            if( seen_quads.insert( om_addr ).second ) {
                quads_by_segment[project_to<coords::seg>( om_addr )].push_back( om_addr );
            }
        }
        for( const auto &cold : cold_quads ) {
            quads_by_segment[project_to<coords::seg>( cold.first )].push_back( cold.first );
        }
    }

    std::list<tripoint_abs_sm> submaps_to_delete;
    static constexpr std::chrono::milliseconds update_interval( 500 );
    std::chrono::steady_clock::time_point last_update = std::chrono::steady_clock::now();

    // First, on this thread, serialize whatever changed: that reads the game state.
    std::vector<segment_write> segments;
    segments.reserve( quads_by_segment.size() );
    for( const auto &[segment_addr, quads] : quads_by_segment ) {
        segment_write &segment = segments.emplace_back();
        segment.dirname = segment_dirname( segment_addr );
        const cata_path &dirname = segment.dirname;

        // Only opened to check what a segment holds, and only when that matters.
        std::optional<zzip> z;
        const auto open_zzip = [&]() -> zzip & {
            if( !z ) {
                cata_path zzip_name = dirname;
                zzip_name += zzip_suffix;
                z = zzip::load( zzip_name.get_unrelative_path(), maps_dictionary_path().get_unrelative_path() );
                if( !z ) {
                    throw std::runtime_error( "Failed opening compressed save file " +
                                              zzip_name.get_unrelative_path().generic_u8string() );
                }
            }
            return *z;
        };

        for( const tripoint_abs_omt &om_addr : quads ) {
            std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
            if( last_update + update_interval < now ) {
                popup.message( _( "Please wait as the map saves [%d/%d]" ),
                               num_saved_submaps, num_total_submaps );
                ui_manager::redraw();
                refresh_display();
                inp_mngr.pump_events();
                last_update = now;
            }
            num_saved_submaps += 4;

            const std::filesystem::path file_name = std::filesystem::u8path( quad_file_name( om_addr ) );
            const cata_path quad_path = dirname / file_name;
            const std::string hash_key = quad_path.generic_u8string();

            const auto cold = cold_quads.find( om_addr );
            if( cold != cold_quads.end() ) {
                // Compacted, so already serialized, and outside the reality bubble.
                const auto hash_it = quad_disk_hashes.find( hash_key );
                const bool unchanged = hash_it != quad_disk_hashes.end() &&
                                       hash_it->second == cold->second.hash;
                quad_disk_hashes.erase( hash_key );
                if( !unchanged ) {
                    segment.files.emplace_back( file_name,
                                                decompress_quad( cold->second.compressed, cold->second.size ) );
                }
                segment.saved_cold_quads.push_back( om_addr );
                continue;
            }

            // delete_on_save deletes everything, otherwise delete submaps
            // outside the current map.
            const bool delete_quad = delete_after_save || !here.inbounds( om_addr );

            std::vector<tripoint_abs_sm> present_addrs;
            present_addrs.reserve( 4 );
            bool all_uniform = true;
            bool any_reverted = false;
            for( const point_rel_sm &offset : {
                     point_rel_sm::zero, point_rel_sm::south, point_rel_sm::east, point_rel_sm::south_east
                 } ) {
                tripoint_abs_sm submap_addr = project_to<coords::sm>( om_addr );
                submap_addr += offset.raw(); // TODO: Make += etc. available to relative parameters as well.
                const auto it = submaps.find( submap_addr );
                if( it == submaps.end() || it->second == nullptr ) {
                    continue;
                }
                present_addrs.push_back( submap_addr );
                if( !it->second->is_uniform() ) {
                    all_uniform = false;
                } else if( it->second->reverted ) {
                    any_reverted = true;
                }
            }

            if( delete_quad ) {
                submaps_to_delete.insert( submaps_to_delete.end(), present_addrs.begin(),
                                          present_addrs.end() );
            }

            // A quad that reverted to uniform has a stale file to get rid of. Checking for
            // that file is costly without a zzip, so it's only done when it matters.
            // The number of uniform submaps is so enormous that the filesystem overhead
            // for this step of just checking if the quad exists approaches 70% of the
            // total cost of saving the mapbuffer, in one test save I had.
            const bool remove_file = all_uniform && any_reverted &&
                                     ( compressed ? open_zzip().has_file( file_name ) :
                                       std::filesystem::exists( quad_path.get_unrelative_path() ) );
            if( all_uniform && !remove_file ) {
                // Nothing to save - this quad will be regenerated faster than it would be re-read
                continue;
            }

            std::string s = serialize_quad( present_addrs );

            if( remove_file ) {
                quad_disk_hashes.erase( hash_key );
                // deleting the file might fail on some platforms in some edge cases so
                // force serialize this uniform quad before deleting it
                segment.files.emplace_back( file_name, std::move( s ) );
                segment.removed.insert( file_name );
                continue;
            }

            const uint64_t hash = quad_content_hash( s );
            const auto hash_it = quad_disk_hashes.find( hash_key );
            const bool unchanged = hash_it != quad_disk_hashes.end() && hash_it->second == hash;
            if( delete_quad ) {
                // The quad leaves memory; it will be hashed again if it is loaded again.
                quad_disk_hashes.erase( hash_key );
            }
            if( unchanged ) {
                // Already on disk exactly as it is now.
                continue;
            }
            segment.files.emplace_back( file_name, std::move( s ) );
            if( !delete_quad ) {
                // Only trusted once the segment is actually written.
                segment.written_hashes.emplace_back( hash_key, hash );
            }
        }
    }

    // Then compress and write the segments in parallel: each is a file of its own, and
    // this part is most of the cost of a save that changed a lot.
    std::vector<size_t> to_write;
    for( size_t i = 0; i < segments.size(); ++i ) {
        if( !segments[i].files.empty() || !segments[i].removed.empty() ) {
            to_write.push_back( i );
        }
    }
    std::vector<std::exception_ptr> failures( segments.size() );
    {
        const std::filesystem::path dictionary = maps_dictionary_path().get_unrelative_path();
        std::atomic<size_t> next_write{ 0 };
        const auto write_segments = [&]() {
            for( size_t i = next_write++; i < to_write.size(); i = next_write++ ) {
                try {
                    write_segment( segments[to_write[i]], compressed, dictionary );
                } catch( ... ) {
                    failures[to_write[i]] = std::current_exception();
                }
            }
        };
        const size_t thread_count = std::min<size_t>( { to_write.size(), max_save_threads,
                                    std::max( 1U, std::thread::hardware_concurrency() )
                                                      } );
        std::vector<std::thread> helpers;
        for( size_t i = 1; i < thread_count; ++i ) {
            helpers.emplace_back( write_segments );
        }
        write_segments();
        for( std::thread &helper : helpers ) {
            helper.join();
        }
    }

    // Finally record what reached the disk.
    std::exception_ptr first_failure;
    for( size_t i = 0; i < segments.size(); ++i ) {
        if( failures[i] ) {
            if( !first_failure ) {
                first_failure = failures[i];
            }
            continue;
        }
        for( const std::pair<std::string, uint64_t> &written : segments[i].written_hashes ) {
            quad_disk_hashes[written.first] = written.second;
        }
        for( const tripoint_abs_omt &om_addr : segments[i].saved_cold_quads ) {
            cold_quads.erase( om_addr );
        }
    }
    if( first_failure ) {
        // Nothing is unloaded, so what failed to save is still here for the next try.
        std::rethrow_exception( first_failure );
    }
    for( auto &elem : submaps_to_delete ) {
        remove_submap( elem );
    }
}

std::string mapbuffer::serialize_quad( const std::vector<tripoint_abs_sm> &submap_addrs )
{
    std::stringstream stringout;
    JsonOut jsout( stringout );
    jsout.start_array();
    for( const tripoint_abs_sm &submap_addr : submap_addrs ) {
        jsout.start_object();

        jsout.member( "version", savegame_version );
        jsout.member( "coordinates" );

        jsout.start_array();
        jsout.write( submap_addr.x() );
        jsout.write( submap_addr.y() );
        jsout.write( submap_addr.z() );
        jsout.end_array();

        submaps[submap_addr]->store( jsout );

        jsout.end_object();
    }
    jsout.end_array();
    return std::move( stringout ).str();
}

void mapbuffer::compact_far_quads()
{
    constexpr size_t reality_bubble_submaps = MAPSIZE * MAPSIZE * OVERMAP_LAYERS;
    if( submaps.size() < reality_bubble_submaps + far_submaps_before_compacting ) {
        return;
    }

    map &here = get_map();
    std::vector<tripoint_abs_omt> far_quads;
    {
        std::set<tripoint_abs_omt> seen_quads;
        for( auto &elem : submaps ) {
            const tripoint_abs_omt om_addr = project_to<coords::omt>( elem.first );
            if( seen_quads.insert( om_addr ).second && !here.inbounds( om_addr ) ) {
                far_quads.push_back( om_addr );
            }
        }
    }

    size_t compacted = 0;
    for( const tripoint_abs_omt &om_addr : far_quads ) {
        if( compacted >= max_quads_compacted_per_call ) {
            break;
        }
        std::vector<tripoint_abs_sm> present_addrs;
        bool all_uniform = true;
        bool any_reverted = false;
        for( const point_rel_sm &offset : {
                 point_rel_sm::zero, point_rel_sm::south, point_rel_sm::east, point_rel_sm::south_east
             } ) {
            const tripoint_abs_sm submap_addr = project_to<coords::sm>( om_addr ) + offset.raw();
            const auto it = submaps.find( submap_addr );
            if( it == submaps.end() || it->second == nullptr ) {
                continue;
            }
            present_addrs.push_back( submap_addr );
            if( !it->second->is_uniform() ) {
                all_uniform = false;
            } else if( it->second->reverted ) {
                any_reverted = true;
            }
        }
        if( any_reverted ) {
            // Its stale file is removed by save(), which needs the live submaps for that.
            continue;
        }
        if( !all_uniform ) {
            std::string content = serialize_quad( present_addrs );
            cold_quad &cold = cold_quads[om_addr];
            cold.hash = quad_content_hash( content );
            cold.size = content.size();
            cold.compressed = compress_quad( content );
        }
        // Uniform quads are dropped as save() would: they regenerate faster than they load.
        for( const tripoint_abs_sm &submap_addr : present_addrs ) {
            submaps.erase( submap_addr );
        }
        ++compacted;
    }
}

bool mapbuffer::thaw_quad( const tripoint_abs_sm &p )
{
    const tripoint_abs_omt om_addr = project_to<coords::omt>( p );
    const auto cold = cold_quads.find( om_addr );
    if( cold == cold_quads.end() ) {
        return false;
    }
    // Out of cold_quads first: deserializing adds the submaps, which thaws their quad.
    cold_quad thawing = std::move( cold->second );
    cold_quads.erase( cold );
    try {
        deserialize( json_loader::from_string( decompress_quad( thawing.compressed, thawing.size ) ) );
    } catch( ... ) {
        // Keep the quad as it was rather than lose it.
        for( const point_rel_sm &offset : {
                 point_rel_sm::zero, point_rel_sm::south, point_rel_sm::east, point_rel_sm::south_east
             } ) {
            submaps.erase( project_to<coords::sm>( om_addr ) + offset.raw() );
        }
        cold_quads.emplace( om_addr, std::move( thawing ) );
        throw;
    }
    // Uniform submaps were not kept, as in a saved quad.
    generate_uniform_omt( project_to<coords::sm>( om_addr ), overmap_buffer.ter( om_addr ) );
    return true;
}

// We're reading in way too many entities here to mess around with creating sub-objects and
// seeking around in them, so we're using the json streaming API.
submap *mapbuffer::unserialize_submaps( const tripoint_abs_sm &p )
{
    // Map the tripoint to the submap quad that stores it.
    const tripoint_abs_omt om_addr = project_to<coords::omt>( p );
    const cata_path dirname = find_dirname( om_addr );
    std::string file_name = quad_file_name( om_addr );
    std::filesystem::path file_name_path = std::filesystem::u8path( file_name );
    cata_path quad_path = dirname / file_name;

    bool read = [&] {
        if( world_generator->active_world->has_compression_enabled() )
        {
            cata_path zzip_name = dirname;
            zzip_name += zzip_suffix;
            if( !file_exist( zzip_name ) ) {
                return false;
            }

            std::optional<zzip> z = zzip::load( zzip_name.get_unrelative_path(),
                                                maps_dictionary_path().get_unrelative_path() );
            if( !z ) {
                debugmsg( _fmt( "Failed to load submaps from {0}, could not open zzip.", zzip_name ) );
                return false;
            }
            if( !z->has_file( file_name_path ) ) {
                return false;
            }
            std::vector<std::byte> contents = z->get_file( file_name_path );
            std::string string_contents{ reinterpret_cast<char *>( contents.data() ), contents.size() };
            try {
                deserialize( json_loader::from_string( string_contents ) );
            } catch( std::exception &err ) {
                debugmsg( _( "Failed to read from \"%1$s\": %2$s" ), zzip_name.generic_u8string() + ":" + file_name,
                          err.what() );
                return false;
            }
            quad_disk_hashes[quad_path.generic_u8string()] = quad_content_hash( string_contents );
            return true;
        } else
        {
            if( !file_exist( quad_path ) ) {
                return false;
            }
            std::optional<std::string> contents = read_whole_file( quad_path );
            if( !contents ) {
                debugmsg( _( "Failed to read from \"%1$s\"" ), quad_path.generic_u8string() );
                return false;
            }
            try {
                deserialize( json_loader::from_string( *contents ) );
            } catch( std::exception &err ) {
                debugmsg( _( "Failed to read from \"%1$s\": %2$s" ), quad_path.generic_u8string(),
                          err.what() );
                return false;
            }
            quad_disk_hashes[quad_path.generic_u8string()] = quad_content_hash( *contents );
            return true;
        }
    }();

    if( !read ) {
        return nullptr;
    }

    // fill in uniform submaps that were not serialized. Note that failure as a result of it
    // not being uniform is OK and results in any missing uniform submaps being generated.
    oter_id const oid = overmap_buffer.ter( om_addr );
    generate_uniform_omt( project_to<coords::sm>( om_addr ), oid );
    if( submaps.count( p ) == 0 ) {
        debugmsg( "file %s did not contain the expected submap %s for non-uniform terrain %s",
                  quad_path.generic_u8string(), p.to_string(), oid.id().str() );
    }

    return submaps[ p ].get();
}

void mapbuffer::deserialize( const JsonArray &ja )
{
    for( JsonObject submap_json : ja ) {
        std::unique_ptr<submap> sm = std::make_unique<submap>();
        tripoint_abs_sm submap_coordinates;
        int version = 0;
        // We have to read version first because the iteration order of json members is undefined.
        if( submap_json.has_int( "version" ) ) {
            version = submap_json.get_int( "version" );
        }
        for( JsonMember submap_member : submap_json ) {
            std::string submap_member_name = submap_member.name();
            if( submap_member_name == "coordinates" ) {
                JsonArray coords_array = submap_member;
                tripoint_abs_sm loc{ coords_array.next_int(), coords_array.next_int(), coords_array.next_int() };
                submap_coordinates = loc;
            } else {
                sm->load( submap_member, submap_member_name, version );
            }
        }

        if( !add_submap( submap_coordinates, sm ) ) {
            debugmsg( "submap %s was already loaded", submap_coordinates.to_string() );
        }
    }
}
