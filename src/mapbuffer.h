#pragma once
#ifndef CATA_SRC_MAPBUFFER_H
#define CATA_SRC_MAPBUFFER_H

#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "coordinates.h"

class JsonArray;
class cata_path;
class submap;

/**
 * Store, buffer, save and load the entire world map.
 */
class mapbuffer
{
    public:
        mapbuffer();
        ~mapbuffer();

        /** Store all submaps in this instance into savefiles.
         * @param delete_after_save If true, the saved submaps are removed
         * from the mapbuffer (and deleted).
         **/
        void save( bool delete_after_save = false );

        /** Delete all buffered submaps. **/
        void clear();

        /**
         * Keep quads outside the reality bubble serialized and compressed in memory instead
         * of as live submaps, once enough of them pile up, to bound memory while exploring
         * a big world. Nothing is written to disk until save(); a compacted quad becomes
         * live again as soon as it is looked up. Call only between turns, when nothing
         * holds a pointer to a submap outside the reality bubble.
         */
        void compact_far_quads();

        /** Delete all buffered submaps except those inside the reality bubble.
         *
         * This exists for the sake of the tests to reduce their memory
         * consumption; it's probably not sane to use in general gameplay.
         */
        void clear_outside_reality_bubble();

        /** Add a new submap to the buffer.
         *
         * @param p The absolute world position in submap coordinates.
         * Same as the ones in @ref lookup_submap.
         * @param sm The submap. If the submap has been added, the unique_ptr
         * is released (set to NULL).
         * @return true if the submap has been stored here. False if there
         * is already a submap with the specified coordinates. The submap
         * is not stored and the given unique_ptr retains ownsership.
         */
        bool add_submap( const tripoint_abs_sm &p, std::unique_ptr<submap> &sm );
        // Old overload that we should stop using, but it's complicated
        bool add_submap( const tripoint_abs_sm &p, submap *sm );

        /** Get a submap stored in this buffer.
         *
         * @param p The absolute world position in submap coordinates.
         * Same as the ones in @ref add_submap.
         * @return NULL if the submap is not in the mapbuffer
         * and could not be loaded. The mapbuffer takes care of the returned
         * submap object, don't delete it on your own.
         */
        submap *lookup_submap( const tripoint_abs_sm &p );
        // Cheaper version of the above for when you only care about whether the
        // submap exists or not.
        bool submap_exists( const tripoint_abs_sm &p );

        // Cheaper version of the above for when you don't mind some false results
        bool submap_exists_approx( const tripoint_abs_sm &p );

    private:
        using submap_map_t = std::map<tripoint_abs_sm, std::unique_ptr<submap>>;

    public:
        inline submap_map_t::iterator begin() {
            return submaps.begin();
        }
        inline submap_map_t::iterator end() {
            return submaps.end();
        }

    private:
        // There's a very good reason this is private,
        // if not handled carefully, this can erase in-use submaps and crash the game.
        void remove_submap( const tripoint_abs_sm &addr );
        submap *unserialize_submaps( const tripoint_abs_sm &p );
        bool submap_file_exists( const tripoint_abs_sm &p );
        void deserialize( const JsonArray &ja );
        std::string serialize_quad( const std::vector<tripoint_abs_sm> &submap_addrs );
        // Makes the quad holding p live again if it was compacted. Returns whether it was.
        bool thaw_quad( const tripoint_abs_sm &p );
        submap_map_t submaps; // NOLINT(cata-serialize)

        // A quad compacted by compact_far_quads(), as it would be saved.
        struct cold_quad {
            std::string compressed;
            size_t size = 0;
            uint64_t hash = 0;
        };
        std::unordered_map<tripoint_abs_omt, cold_quad> cold_quads; // NOLINT(cata-serialize)
        /**
         * Hash of each quad file's contents as last read from or written to disk, keyed by
         * the quad's full path. save() skips a quad that serializes to the same hash, which
         * avoids recompressing and rewriting the many quads that did not change since.
         */
        std::unordered_map<std::string, uint64_t> quad_disk_hashes; // NOLINT(cata-serialize)
};

extern mapbuffer MAPBUFFER;

#endif // CATA_SRC_MAPBUFFER_H
