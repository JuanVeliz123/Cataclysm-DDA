#pragma once
#if defined(GODOT)

#include "coordinates.h"
#include "godot_backend.h"
#include "map_scale_constants.h"
#include "point.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace godot_backend
{

/// Pixel-space rectangle (analog of SDL_Rect for the Godot backend).
struct pixel_minimap_rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

enum class pixel_minimap_type : int {
    ortho,
    iso
};

enum class pixel_minimap_mode : int {
    solid,
    squares,
    dots
};

struct pixel_minimap_settings {
    pixel_minimap_mode mode = pixel_minimap_mode::solid;
    int brightness = 100;
    int beacon_size = 2;
    int beacon_blink_interval = 0;
    bool square_pixels = true;
    bool scale_to_fit = false;
};

/// Maps game tile coordinates to minimap pixel coordinates. Godot port of
/// src/pixel_minimap_projectors.{h,cpp}; SDL_Rect is replaced by
/// @ref pixel_minimap_rect so no SDL types leak into the Godot build.
class pixel_minimap_projector
{
    public:
        pixel_minimap_projector() = default;
        virtual ~pixel_minimap_projector() = default;

        virtual point get_tile_size() const = 0;
        virtual point get_tiles_size( const point &tiles_count ) const = 0;
        virtual point get_tile_pos( const point &p, const point &tiles_count ) const = 0;

        virtual pixel_minimap_rect get_chunk_rect( const point &p,
                const point &tiles_count ) const = 0;
};

class pixel_minimap_ortho_projector : public pixel_minimap_projector
{
    public:
        pixel_minimap_ortho_projector( const point &total_tiles_count,
                                       const pixel_minimap_rect &max_screen_rect,
                                       bool square_pixels );

        point get_tile_size() const override;
        point get_tiles_size( const point &tiles_count ) const override;
        point get_tile_pos( const point &p, const point &tiles_count ) const override;

        pixel_minimap_rect get_chunk_rect( const point &p,
                                           const point &tiles_count ) const override;

    private:
        point tile_size;
};

class pixel_minimap_iso_projector : public pixel_minimap_projector
{
    public:
        pixel_minimap_iso_projector( const point &total_tiles_count,
                                     const pixel_minimap_rect &max_screen_rect,
                                     bool square_pixels );

        point get_tile_size() const override;
        point get_tiles_size( const point &tiles_count ) const override;
        point get_tile_pos( const point &p, const point &tiles_count ) const override;

        pixel_minimap_rect get_chunk_rect( const point &p,
                                           const point &tiles_count ) const override;

    private:
        point total_tiles_count;
        point tile_size;
};

class GodotPixelMinimap
{
    public:
        GodotPixelMinimap();
        ~GodotPixelMinimap();

        void set_type( pixel_minimap_type type );
        void set_settings( const pixel_minimap_settings &settings );

        /**
         * Render the minimap at @p size_px into the internal RGBA8 buffer.
         *
         * Game-thread only: walks the map, the submap cache and the creature
         * tracker. The Godot side picks the result up with @ref copy_rgba.
         */
        void draw( const point &size_px, const tripoint_bub_ms &center );
        void reset();

        /// Copy the last rendered frame as RGBA8. Empty until the first @ref draw.
        /// Safe to call from the Godot main thread.
        std::vector<uint8_t> copy_rgba( int &width, int &height ) const;

        /// Bumped by each @ref draw that produces a frame, so the Godot side can
        /// skip rebuilding an unchanged texture.
        uint64_t generation() const;

        // True if the last draw() rendered any critters with blinking beacons.
        bool has_blinking_beacons() const {
            return has_blinking_beacons_;
        }

    private:
        // the color stored for each submap tile
        struct submap_cache {
            // flat array of SEEX * SEEY RGBA-packed colors
            std::vector<uint32_t> minimap_colors;
            // RGBA8 pixels of this chunk, chunk_size.x * chunk_size.y * 4 bytes.
            // A plain buffer rather than a godot::Image: this is only ever a CPU
            // scratch surface for render_cache, and it is filled on the game
            // thread, where creating Godot objects buys nothing.
            std::vector<uint8_t> chunk_pixels;
            // checks if the submap has been looked at by the minimap routine
            bool touched = false;
            // the list of updates to apply to the chunk image
            // reduces image uploads to once per submap
            std::vector<point> update_list;
            // flag used to indicate that the chunk image needs to be cleared before first use
            bool ready = false;

            explicit submap_cache( const point &chunk_size );
            submap_cache() = default;

            uint32_t &color_at( const point &p );
            const uint32_t &color_at( const point &p ) const;
        };

        submap_cache &get_cache_at( const tripoint_abs_sm &abs_sm_pos );

        void set_screen_rect( const pixel_minimap_rect &screen_rect );

        void draw_beacon( const pixel_minimap_rect &rect, const color &c,
                          uint8_t *data, int width, int height );

        void process_cache( const tripoint_bub_ms &center );

        void flush_cache_updates();
        void update_cache_at( const tripoint_bub_sm &pos );
        void prepare_cache_for_updates( const tripoint_bub_ms &center );
        void clear_unused_cache();

        void render( const tripoint_bub_ms &center );
        void render_cache( const tripoint_bub_ms &center, uint8_t *data, int width, int height );
        void render_critters( const tripoint_bub_ms &center, uint8_t *data, int width, int height );

        std::unique_ptr<pixel_minimap_projector> create_projector(
            const pixel_minimap_rect &max_screen_rect ) const;

        pixel_minimap_type type;
        pixel_minimap_settings settings;

        point pixel_size;

        // the pixel size of one submap chunk image
        point chunk_size;

        // track the previous viewing area to determine if the minimap cache needs to be cleared
        tripoint_abs_sm cached_center_sm;

        pixel_minimap_rect screen_rect;
        pixel_minimap_rect main_tex_clip_rect;
        pixel_minimap_rect screen_clip_rect;

        // The main compositing surface, RGBA8, main_size.x * main_size.y * 4.
        std::vector<uint8_t> main_pixels;
        point main_size;

        // Published copy of the last completed frame. Written on the game thread
        // under the mutex, read by the Godot main thread; the render buffer itself
        // is mid-update for most of a frame and must not be handed out directly.
        mutable std::mutex out_mutex_;
        std::vector<uint8_t> out_pixels_;
        point out_size_;
        uint64_t generation_ = 0;

        std::unique_ptr<pixel_minimap_projector> projector;

        std::unordered_map<tripoint_abs_sm, submap_cache> cache;

        bool has_blinking_beacons_ = false;
};

/// The single process-lifetime minimap, mirroring get_map_snapshot().
GodotPixelMinimap &get_pixel_minimap();

/**
 * Re-render the minimap around the avatar. Game-thread only; called once per turn
 * beside update_map_snapshot().
 *
 * A no-op until the Godot side has asked for a size with
 * @ref set_pixel_minimap_size, so nothing is rendered for a panel nobody shows.
 */
void update_pixel_minimap();

/// Ask for a minimap of @p size_px pixels. Called from the Godot main thread when
/// the minimap panel is laid out; a zero size disables rendering.
void set_pixel_minimap_size( const point &size_px );

} // namespace godot_backend

#endif // GODOT
