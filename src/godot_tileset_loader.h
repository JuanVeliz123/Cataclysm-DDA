#pragma once
#ifndef CATA_SRC_GODOT_TILESET_LOADER_H
#define CATA_SRC_GODOT_TILESET_LOADER_H

#if defined(GODOT)

#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cata_path.h"
#include "cuboid_rectangle.h"
#include "godot_backend.h"
#include "point.h"
#include "weighted_list.h"

class JsonObject;

namespace godot_backend
{

/// Contextual layering sprite record, mirroring layer_context_sprites from the
/// SDL backend (src/cata_tiles.h) without pulling in SDL headers.
struct godot_layer_context_sprites {
    std::string id;
    std::map<std::string, int> sprite;
    int layer = 0;
    point offset = point::zero;
    int total_weight = 0;
    std::string append_suffix;
};

/// One tile definition: weighted sprite lists plus rendering hints, mirroring
/// tile_type from the SDL backend (src/cata_tiles.h).
struct godot_tile_type {
    weighted_int_list<std::vector<int>> fg;
    weighted_int_list<std::vector<int>> bg;
    bool multitile = false;
    bool rotates = false;
    bool animated = false;
    int height_3d = 0;
    point offset = point::zero;
    point offset_retracted = point::zero;
    float pixelscale = 1.0;
    std::vector<std::string> available_subtiles;
};

/**
 * A colour ramp a sprite can be recoloured through (SP-8).
 *
 * Two zombie variants that differ only in colour do not need two sprites. Draw
 * one, take its luminance, and use that to index a ramp: the sprite becomes a
 * grayscale master and the variant becomes six colours in a JSON file. The
 * shader does the lookup, so the cost at runtime is one extra texture fetch on
 * the handful of tiles that opt in.
 */
struct godot_palette {
    std::string id;
    /// Dark to light. Sampled as a 1-D texture, so three entries give a coarse
    /// ramp and sixteen give a smooth one.
    std::vector<std::array<uint8_t, 3>> ramp;
};

/// One id's recolouring, from the "variants" section of the palette file.
struct godot_sprite_variant {
    /// Sprite id to draw instead of this one, or "" to keep its own.
    std::string sprite;
    /// 1-based row in @ref godot_tileset::palettes; 0 means no recolouring.
    int palette_row = 0;
};

/**
 * The Godot-backend result of loading a tileset: tile id -> definitions plus the
 * sprite array. Textures are godot::ImageTexture-backed sprites sharing per-atlas
 * textures that @ref atlas_textures keeps alive.
 *
 * Unlike the SDL tileset class (src/cata_tiles.h) there is one atlas, not six:
 * lighting is applied as a GPU tint rather than by swapping to a pre-filtered
 * copy of every sprite. See ADR-003.
 */
class godot_tileset
{
    public:
        void clear();

        bool is_isometric() const {
            return tile_isometric_;
        }
        int get_tile_width() const {
            return tile_width_;
        }
        int get_tile_height() const {
            return tile_height_;
        }
        const half_open_rectangle<point> &get_max_tile_extent() const {
            return max_tile_extent_;
        }
        int get_zlevel_height() const {
            return zlevel_height_;
        }
        float get_tile_pixelscale() const {
            return tile_pixelscale_;
        }
        float get_prevent_occlusion_min_dist() const {
            return prevent_occlusion_min_dist_;
        }
        float get_prevent_occlusion_max_dist() const {
            return prevent_occlusion_max_dist_;
        }
        const std::string &get_tileset_id() const {
            return tileset_id_;
        }

        godot_tile_type &create_tile_type( const std::string &id, godot_tile_type &&new_tile_type );
        const godot_tile_type *find_tile_type( const std::string &id ) const;
        std::optional<int> get_default_item_highlight_index() const {
            return default_item_highlight_index_;
        }
        void set_default_item_highlight_index( std::optional<int> idx ) {
            default_item_highlight_index_ = idx;
        }

        std::unordered_map<std::string, godot_tile_type> tile_ids;
        std::unordered_set<std::string> duplicate_ids;
        /// The sprites. Only the unfiltered atlas exists: lighting is a GPU tint
        /// (ADR-003), not one pre-filtered atlas copy per light level as in SDL.
        std::vector<godot_texture> tile_values;
        // Owning refs to every per-atlas, per-variant ImageTexture so the
        // sprites above (which only hold non-owning sub-rects) stay valid.
        std::vector<godot::Ref<godot::ImageTexture>> atlas_textures;
        std::unordered_map<std::string, std::vector<godot_layer_context_sprites>> item_layer_data;
        std::unordered_map<std::string, std::vector<godot_layer_context_sprites>> field_layer_data;

        /// Palette-swap data (SP-8), loaded from godot_palettes.json.
        std::vector<godot_palette> palettes;
        /// Sprite id -> its redirect and palette row.
        std::unordered_map<std::string, godot_sprite_variant> sprite_variants;
        /// The palettes as one RGBA8 Image: one row per palette, widest ramp
        /// wide, for the shader to index by luminance. Null when none loaded.
        godot::Ref<godot::Image> build_palette_image() const;

    private:
        friend class godot_tileset_loader;
        std::string tileset_id_;
        bool tile_isometric_ = false;
        int tile_width_ = 0;
        int tile_height_ = 0;
        half_open_rectangle<point> max_tile_extent_;
        int zlevel_height_ = 0;
        float tile_pixelscale_ = 1.0f;
        float prevent_occlusion_min_dist_ = -1.0f;
        float prevent_occlusion_max_dist_ = 0.0f;
        // Sprite index of the synthetic highlight overlay, or nullopt when the
        // tileset defines its own ITEM_HIGHLIGHT.
        std::optional<int> default_item_highlight_index_;
};

/**
 * Tileset loader for the Godot backend.
 *
 * Parses the exact same JSON as the SDL loader (src/tileset_loader.cpp) --
 * tile_config.json, tile ids, tile_type records, subtiles, weighted lists,
 * ASCII sections and layering data -- but loads the atlas PNGs into
 * godot::Image and uploads them through godot::ImageTexture instead of SDL
 * surfaces/textures. The SDL loader is untouched; this is a parallel
 * implementation for the Godot build.
 */
class godot_tileset_loader
{
    public:
        /// tileset_id matches the option string. precheck only parses metadata
        /// (tile dimensions) and skips texture upload. pump_events mirrors the
        /// SDL loader's signature (a no-op until the Godot input bridge lands).
        /// terrain marks the overmap/terrain tileset for the "unknown_terrain"
        /// fallback check.
        void load( godot_tileset &result, const std::string &tileset_id,
                   bool precheck, bool pump_events = false, bool terrain = false );

    private:
        godot_tileset *ts = nullptr;

        // Per-atlas parse state (mirrors tileset_cache::loader).
        point sprite_offset;
        point sprite_offset_retracted;
        float sprite_pixelscale = 1.0;
        int sprite_width = 0;
        int sprite_height = 0;
        int offset = 0;
        int sprite_id_offset = 0;
        int size = 0;
        int R = -1;
        int G = -1;
        int B = -1;
        int tile_atlas_width = 0;

        // Atlas data gathered during the parse pass; the decoded image is kept
        // alive so upload_atlases can filter and upload it without re-decoding.
        struct atlas_descriptor {
            cata_path image_path;
            int color_key_r = -1;
            int color_key_g = -1;
            int color_key_b = -1;
            int sprite_width = 0;
            int sprite_height = 0;
            int atlas_offset = 0;
            int expected_tilecount = 0;
            godot::Ref<godot::Image> image;
        };
        std::vector<atlas_descriptor> atlases_;

        // Walk every atlas in config (tiles-new array, or the single-atlas
        // fallback when no tiles-new is present) running
        // read_image_dimensions + parse_mappings on each.
        void parse_atlases( const JsonObject &config, const cata_path &tileset_root,
                            const cata_path &img_path, bool pump_events );
        // Load the atlas image, record its dimensions and append an
        // atlas_descriptor. No GPU work. kr/kg/kb identify the transparent
        // color; all-negative disables color keying.
        void read_image_dimensions( const cata_path &img_path, int kr, int kg, int kb );
        // Parse tile-id -> sprite-index mappings from a tileset JSON object.
        // Indices in fg/bg are validated against [0,size) and shifted by
        // offset.
        void parse_mappings( const JsonObject &config );
        // Create a new tile_type, add it to tile_ids (using id). Sets fg/bg
        // properties from the json object.
        godot_tile_type &load_tile( const JsonObject &entry, const std::string &id );
        void load_tile_spritelists( const JsonObject &entry,
                                    weighted_int_list<std::vector<int>> &vs,
                                    std::string_view objname ) const;
        void load_ascii( const JsonObject &config );
        void load_ascii_set( const JsonObject &entry );
        void add_ascii_subtile( godot_tile_type &curr_tile, const std::string &t_id,
                                int sprite_id, const std::string &s_id );
        // Load layering data from json.
        void load_layers( const JsonObject &config );
        /// Read godot_palettes.json, preferring the tileset's own copy over the
        /// shared one in data/godot/. Absent is not an error: a tileset with no
        /// palettes simply draws every sprite in its own colours.
        void load_palettes( const cata_path &tileset_root );
        void process_variations_after_loading( weighted_int_list<std::vector<int>> &vs ) const;
        // Upload every recorded atlas: decode/keep the image in
        // read_image_dimensions, apply the color key, emit the six color-filter
        // variant textures and fill the per-sprite texture arrays.
        /// Decode each atlas and slice it into sprites. Takes no memory-map mode:
        /// there is no memory atlas variant to filter any more (ADR-003).
        void upload_atlases();
};

} // namespace godot_backend

#endif // GODOT

#endif // CATA_SRC_GODOT_TILESET_LOADER_H
