#pragma once
#ifndef CATA_SRC_GODOT_MAP_SNAPSHOT_H
#define CATA_SRC_GODOT_MAP_SNAPSHOT_H

#if defined(GODOT)

#include <array>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/vector2i.hpp>

namespace godot_backend
{

/// Render flags packed into @ref map_draw_cmd::rot_flags above the two rotation
/// bits. They ride along in an existing field rather than widening the command,
/// because the stride is a contract with map_view.gd's CMD_STRIDE.
enum cmd_flag : int32_t {
    /// Low two bits: quarter turns clockwise, 0-3.
    cmd_rotation_mask = 0x3,
    /// Vertex sway (SP-7). Grass and foliage bend in the wind; walls do not.
    cmd_flag_sway = 1 << 2,
    /// Mirror horizontally about the sprite's centre. This is how a character
    /// faces left: SDL passes rota = -1 to render_copy_ex, which flips rather
    /// than rotates, and a quarter-turn field has no way to say that.
    cmd_flag_flip_x = 1 << 3,
    /// Bits 4-7: palette row + 1, or 0 for "draw the sprite's own colours"
    /// (SP-8). Fifteen palettes is more than a tileset is likely to want.
    cmd_palette_shift = 4,
    cmd_palette_mask = 0xF << cmd_palette_shift,
};

/// One draw command for MapView: atlas sub-rect → screen pixels.
/// Packed as 10 ints: atlas, src_x, src_y, src_w, src_h, dest_x, dest_y, layer,
/// tint, rot_flags. Keep @ref MapSnapshot::cmd_stride and map_view.gd's
/// CMD_STRIDE in sync.
struct map_draw_cmd {
    int32_t atlas = 0;
    int32_t src_x = 0;
    int32_t src_y = 0;
    int32_t src_w = 0;
    int32_t src_h = 0;
    int32_t dest_x = 0;
    int32_t dest_y = 0;
    int32_t layer = 0;
    /// Per-sprite modulation as 0xRRGGBBAA, derived from the tile's
    /// @ref lit_level and whether it is remembered rather than seen (ADR-003).
    /// This replaces SDL's trick of pre-tinting a whole second atlas per light
    /// level: the GPU multiplies it in, so the value can be continuous instead
    /// of bucketed into five variants.
    int32_t tint = static_cast<int32_t>( 0xFFFFFFFF );
    /// Rotation in the low two bits, @ref cmd_flag render flags above them.
    ///
    /// The rotation is quarter turns clockwise about the tile centre, 0-3,
    /// mirroring what SDL does with render_copy_ex: 1 is +90 degrees, 2 is 180,
    /// 3 is -90. Connected terrain (walls, roads, fences) needs this to join up.
    int32_t rot_flags = 0;
};

/// A tile whose id resolved to no sprite anywhere in the fallback chain, drawn
/// instead as the symbol its JSON declares (SP-1 step 5). MapView paints these
/// with a font, one layer node per @ref map_layer, so a fallback terrain glyph
/// still sits underneath a monster that did find a sprite.
///
/// Packed as 6 ints: dest_x, dest_y, layer, codepoint, fg, bg. Keep
/// @ref MapSnapshot::glyph_stride and map_view.gd's GLYPH_STRIDE in sync.
struct map_glyph_cmd {
    int32_t dest_x = 0;
    int32_t dest_y = 0;
    int32_t layer = 0;
    int32_t codepoint = 0;
    /// 0xRRGGBBAA, same convention as map_draw_cmd::tint.
    int32_t fg = static_cast<int32_t>( 0xFFFFFFFF );
    /// 0xRRGGBBAA, or 0 for no background fill.
    int32_t bg = 0;
};

/// What a field emits, for the particle layer (SP-6). Decided from the field
/// type's own JSON -- has_fire, and whether its phase is gas -- rather than from
/// a list of ids, so a mod's new smoke behaves like smoke.
enum class field_particle : int32_t {
    fire = 0,
    smoke = 1,
};

/// One field worth animating with particles (SP-6).
///
/// Fire and smoke are the two things on the map that move continuously, and the
/// tileset animates them with a handful of frames at whatever rate the tileset
/// author chose. Particles cost no art, run at the frame rate rather than the
/// turn rate, and can carry an intensity -- which the game already tracks and
/// the spritesheet has no way to express.
///
/// Packed as 4 ints: dest_x, dest_y, kind, intensity. Keep
/// @ref MapSnapshot::field_stride and map_view.gd's FIELD_STRIDE in sync.
struct map_field_cmd {
    int32_t dest_x = 0;
    int32_t dest_y = 0;
    int32_t kind = 0;
    int32_t intensity = 1;
};

/// What an id names. Decides which JSON type supplies its `looks_like`, which
/// "unknown_<category>" placeholder stands in for it, and which symbol the
/// glyph fallback draws.
///
/// This is the subset of TILE_CATEGORY (src/cata_tiles.h) that MapView emits.
/// It is redeclared rather than reused because cata_tiles.h is SDL-only and is
/// deliberately not compiled into the Godot library.
enum class sprite_category : int32_t {
    none = 0,
    terrain,
    furniture,
    trap,
    field,
    vehicle_part,
    item,
    monster,
    character,
    last,
};

/// The tileset id fragment "unknown_<category>" uses. Matches the strings in
/// TILE_CATEGORY_IDS so the placeholders a tileset already ships for SDL work
/// here unchanged.
const char *category_id( sprite_category cat );

/**
 * How far down the fallback chain an id had to go before something could be
 * drawn (SP-1). Reported per id by the coverage report (SP-2) and in aggregate
 * by the debug overlay (SP-9).
 *
 * The order matches cata_tiles::draw_from_id_string, so the Godot renderer
 * picks the same sprite as SDL for the same miss -- with one addition at the
 * end: where SDL gives up and draws "unknown", this can still draw the type's
 * own JSON symbol, which says what the tile actually is.
 */
enum class sprite_fallback : int32_t {
    /// The id is in the tileset.
    exact = 0,
    /// Reached by following `looks_like` through the JSON.
    looks_like = 1,
    /// The tileset's own ASCII sprite for the type's symbol and colour.
    ascii = 2,
    /// "unknown_<category>", or the generic "unknown".
    category = 3,
    /// No sprite at all: drawn as a font glyph from the JSON symbol.
    glyph = 4,
    /// Not even a symbol to draw. Nothing is emitted.
    missing = 5,
    last = 6,
};

/**
 * Draw order. CDDA separates anything that must overlap into its own layer, so
 * MapView can batch each layer independently and still composite correctly.
 *
 * Characters get two layers each, not one. Their overlays -- clothing, the
 * weapon in hand, mutations -- have to land on top of the body, and within a
 * layer MapView batches by atlas, so two sprites of the same layer drawn from
 * different atlases have no guaranteed order between them. Ultica spreads its
 * overlays over five sheets, so that is not a theoretical concern.
 */
enum class map_layer : int32_t {
    terrain_bg = 0,
    terrain_fg = 1,
    furniture = 2,
    trap = 3,
    field = 4,
    vehicle = 5,
    item = 6,
    monster = 7,
    monster_overlay = 8,
    player = 9,
    player_overlay = 10,
};

/// True for the layers a creature draws on, which are the ones whose instances
/// can move between map rebuilds (SP-5 hit reactions).
constexpr bool is_creature_layer( const map_layer layer )
{
    return layer == map_layer::monster || layer == map_layer::monster_overlay ||
           layer == map_layer::player || layer == map_layer::player_overlay;
}

/**
 * Godot-owned tileset present bridge (ADR-002 tileset MapView).
 * Game thread loads UltimateCataclysm (or fallback), builds a draw list;
 * Godot main thread copies atlas Images once and paints the draw list.
 */
class MapSnapshot
{
    public:
        /// Ints per packed command in @ref copy_draw_list.
        static constexpr int cmd_stride = 10;
        /// Ints per packed command in @ref copy_glyph_list.
        static constexpr int glyph_stride = 6;
        /// Ints per packed command in @ref copy_field_list.
        static constexpr int field_stride = 4;

        bool ensure_tileset_loaded( const std::string &tileset_id = "UltimateCataclysm" );
        bool tileset_ready() const;
        std::string tileset_id() const;
        int tile_width() const;
        int tile_height() const;
        int atlas_count() const;

        /// Copy atlas @p index into a Godot Image (RGBA8). Empty on failure.
        godot::Ref<godot::Image> copy_atlas_image( int index ) const;

        /// The tileset's palette ramps as one RGBA8 Image (SP-8): row 0 is the
        /// identity ramp, then one row per palette, indexed by luminance. Null
        /// when the tileset declares no palettes.
        godot::Ref<godot::Image> copy_palette_image() const;

        /// How many tiles the Godot view wants published, from its own viewport
        /// size and zoom. Called from the Godot thread; the game thread reads it
        /// on the next rebuild. Zero means "fall back to the terminal grid".
        void set_requested_view_tiles( int w, int h );
        /// True when the requested extent no longer matches what was published,
        /// so the caller knows a rebuild is worth doing.
        bool view_extent_stale() const;

        /// Drop cached godot::Ref state. Godot main thread, at shutdown.
        void release_resources();

        /// Rebuild draw list from current map / avatar (game thread only).
        void update_from_game();

        godot::PackedInt32Array copy_draw_list() const;
        /// Fallback glyphs for the same frame; see @ref map_glyph_cmd.
        godot::PackedInt32Array copy_glyph_list() const;
        /// Particle-emitting fields for the same frame; see @ref map_field_cmd.
        godot::PackedInt32Array copy_field_list() const;
        godot::Vector2i view_size_tiles() const;
        godot::Vector2i view_origin_tiles() const;
        int command_count() const;
        int glyph_count() const;

        /**
         * Sprite coverage report (SP-2): every id this session had to fall back
         * for, most-drawn first.
         *
         * Thousands of ids can be missing from a tileset and almost none of them
         * matter; what matters is the handful the player is looking at. Ordering
         * by how many draw commands an id produced turns the first into the
         * second. Each entry is a Dictionary of id / category / level /
         * level_name / hits.
         *
         * @param limit at most this many entries, or all of them when <= 0.
         */
        godot::Array copy_sprite_coverage( int limit ) const;

        /// Everything the render debug overlay shows (SP-9): tileset identity,
        /// view extent, command counts per layer and per fallback level.
        godot::Dictionary copy_render_stats() const;

        /**
         * Explain one id: what it resolves to, how, and with which effects
         * (SP-9).
         *
         * "Why did this tile draw what it drew" is otherwise unanswerable from
         * outside -- five fallback levels and a palette redirect all produce a
         * sprite, and four of the five look like art someone chose. This runs
         * the same resolution the renderer runs and reports each step.
         *
         * @param category one of the strings @ref category_id returns.
         * @return { id, category, resolved, level, level_name, palette_row,
         *           codepoint, sways }.
         */
        godot::Dictionary describe_sprite( const std::string &id,
                                           const std::string &category ) const;

        /**
         * The avatar's character overlays, as resolved by the last rebuild.
         *
         * Each entry is { slot, variant, sprite, drawn }: what the game asked
         * for, and what the tileset had for it. `drawn` is false when the
         * chain found nothing, which is the ordinary case for most of the item
         * list and is why a missing garment must not read as an error.
         *
         * The list is in draw order, so reading it top to bottom is reading
         * what is stacked on the character from the skin outward.
         *
         * Snapshotted rather than computed on demand. Deriving it here would
         * mean walking the avatar's effects, mutations and worn items from the
         * Godot thread while the game thread is free to be mutating all three,
         * which is the race this whole class exists to avoid.
         */
        godot::Array copy_avatar_overlays() const;

        /// Bumped by every @ref update_from_game. The Godot side polls this so it
        /// can skip copying and re-batching an unchanged draw list -- MapView
        /// refreshes per frame but the map only changes per turn.
        uint64_t generation() const;

    private:
        mutable std::mutex mutex_;
        bool ready_ = false;
        std::string tileset_id_;
        int tile_w_ = 32;
        int tile_h_ = 32;
        int view_w_ = 0;
        int view_h_ = 0;
        std::atomic<int> req_view_w_{ 0 };
        std::atomic<int> req_view_h_{ 0 };
        int origin_x_ = 0;
        int origin_y_ = 0;
        uint64_t generation_ = 0;
        std::vector<map_draw_cmd> cmds_;
        std::vector<map_glyph_cmd> glyphs_;
        std::vector<map_field_cmd> fields_;

        /// One overlay slot and what it resolved to; see copy_avatar_overlays.
        struct overlay_record {
            std::string slot;
            std::string variant;
            std::string sprite;
            bool drawn = false;
        };
        std::vector<overlay_record> avatar_overlays_;

        /// One id's fallback history, for @ref copy_sprite_coverage.
        struct coverage_entry {
            sprite_category category = sprite_category::none;
            /// Worst (highest) level this id ever needed. An id that resolves
            /// exactly at one call site and by glyph at another is a miss.
            sprite_fallback level = sprite_fallback::exact;
            /// Draw commands this id produced since the tileset was loaded.
            uint64_t hits = 0;
        };
        /// Ids that resolved past @ref sprite_fallback::looks_like only: an
        /// exact hit is the normal case and there are tens of thousands of them.
        std::unordered_map<std::string, coverage_entry> coverage_;
        /// Commands emitted at each @ref sprite_fallback level, this frame.
        std::array<int32_t, static_cast<size_t>( sprite_fallback::last )> fallback_counts_{};
        /// Commands emitted on each @ref map_layer, this frame.
        std::array<int32_t, 16> layer_counts_{};
        // Deduped NORMAL atlas pixel buffers for Godot-side ImageTexture creation.
        struct atlas_pixels {
            int w = 0;
            int h = 0;
            std::vector<uint8_t> rgba;
        };
        std::vector<atlas_pixels> atlases_;
        /// Built once at tileset load; see @ref copy_palette_image.
        /// Palette ramps as raw pixels, not as an Image.
        ///
        /// Deliberately matching @ref atlas_pixels rather than holding a
        /// godot::Ref here. This class has a file-static instance, so a Ref
        /// member is released during static destruction -- after Godot has torn
        /// its ObjectDB down -- and unref() then touches freed memory. The
        /// atlases avoid it the same way and for the same reason.
        atlas_pixels palette_pixels_;
};

MapSnapshot &get_map_snapshot();

bool ensure_tileset_loaded( const std::string &tileset_id = "UltimateCataclysm" );
void update_map_snapshot();

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_MAP_SNAPSHOT_H
