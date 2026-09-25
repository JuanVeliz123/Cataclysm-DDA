#pragma once
#ifndef CATA_SRC_GODOT_OVERMAP_SNAPSHOT_H
#define CATA_SRC_GODOT_OVERMAP_SNAPSHOT_H

#if defined(GODOT)

#include <cstdint>
#include <mutex>
#include <atomic>
#include <string>
#include <vector>

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include "coordinates.h"
#include "godot_map_snapshot.h"

namespace godot_backend
{

/// Draw order for the overmap. Same idea as @ref map_layer: anything that must
/// overlap gets its own layer so the Godot side can batch each independently.
enum class overmap_layer : int32_t {
    terrain = 0,
    map_extra = 1,
    note = 2,
    player = 3,
    cursor = 4,
};

/**
 * Overmap draw list for the Godot OvermapView.
 *
 * The same shape as @ref MapSnapshot, and for the same reason: C++ keeps the id
 * resolution (which needs overmap_buffer, vision levels and the tileset's
 * tile_config) and Godot just paints the resulting sprites.
 *
 * This has its own tileset because the overmap does too -- CDDA draws it from the
 * OVERMAP_TILES option (Larwick_Overmap by default), separate from the map
 * tileset, and its sprites are one-per-overmap-terrain rather than one-per-tile.
 */
class OvermapSnapshot
{
    public:
        /// Drop cached godot::Ref state. Godot main thread, at shutdown.
        void release_resources();

        /// Load the overmap tileset. Falls back to whatever is available under
        /// gfx/ if the configured one is missing.
        bool ensure_tileset_loaded();
        bool tileset_ready() const;
        std::string tileset_id() const;
        int tile_width() const;
        int tile_height() const;
        int atlas_count() const;
        godot::Ref<godot::Image> copy_atlas_image( int index ) const;

        /**
         * Rebuild the draw list around @p center. Game thread only.
         *
         * @param cursor the selection cursor, drawn on top
         * @param blink drives the overlays CDDA blinks (cursor, revealed highlights)
         */
        void update_from_game( const tripoint_abs_omt &center, const tripoint_abs_omt &cursor,
                               bool blink );

        godot::PackedInt32Array copy_draw_list() const;
        godot::Vector2i view_size_tiles() const;
        godot::Vector2i view_origin_tiles() const;
        int command_count() const;
        uint64_t generation() const;

        /// The sidebar's text, recorded from the same functions that draw it.
        /// Its own generation: the map redraws on every blink, the sidebar only
        /// changes when the cursor moves or a setting is toggled.
        struct sidebar_line {
            std::string text;
            std::string color;
            int indent = 0;
            bool join = false;
            bool header = false;
        };
        void publish_sidebar( const std::vector<sidebar_line> &lines );
        godot::Dictionary copy_sidebar() const;
        uint64_t sidebar_generation() const;
        /// Proof a Godot panel is drawing the sidebar. Until one is, the ImGui
        /// window keeps drawing it -- the screen must not be left with neither.
        void note_sidebar_attended();
        bool sidebar_attended() const;

        /// True while the overmap UI is on screen. Set by an RAII guard around
        /// overmap_ui's display loop so the Godot host knows when to show the view.
        bool active() const;
        void set_active( bool active );

    private:
        mutable std::mutex mutex_;
        bool ready_ = false;
        bool active_ = false;
        std::string tileset_id_;
        int tile_w_ = 32;
        int tile_h_ = 32;
        int view_w_ = 0;
        int view_h_ = 0;
        int origin_x_ = 0;
        int origin_y_ = 0;
        uint64_t generation_ = 0;
        std::vector<sidebar_line> sidebar_;
        uint64_t sidebar_generation_ = 0;
        std::atomic<bool> sidebar_attended_{ false };
        std::vector<map_draw_cmd> cmds_;

        struct atlas_pixels {
            int w = 0;
            int h = 0;
            std::vector<uint8_t> rgba;
        };
        std::vector<atlas_pixels> atlases_;
};

OvermapSnapshot &get_overmap_snapshot();

/// RAII guard marking the overmap UI as on screen for its whole lifetime.
class overmap_active_guard
{
    public:
        overmap_active_guard();
        ~overmap_active_guard();

        overmap_active_guard( const overmap_active_guard & ) = delete;
        overmap_active_guard &operator=( const overmap_active_guard & ) = delete;
};

/// Publish the overmap around @p center. Called from overmap_ui's redraw.
void update_overmap_snapshot( const tripoint_abs_omt &center, const tripoint_abs_omt &cursor,
                              bool blink );

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_OVERMAP_SNAPSHOT_H
