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

class Creature;

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
    /// The sprite is larger than its tile cell, so it overhangs its neighbours
    /// and has to draw above the flat sprites of the same layer.
    ///
    /// MapView batches per atlas and cannot interleave two batches by y, so
    /// ordering whole batches puts every sprite of one atlas above every sprite
    /// of another. Within a layer that is wrong for exactly the sprites that
    /// overlap: it drew the ground on top of tree canopies. Splitting tall from
    /// flat gives them separate depth bands, which is right for a fixed-angle
    /// view anyway -- a tree belongs above the grass around its base.
    cmd_flag_tall = 1 << 8,
    /// Bits 9-12: how many z-levels *below* the avatar this sprite stands on,
    /// 0-15 (ADR-005 item 1). Zero is the level the avatar is on, which is the
    /// only value that existed before z-levels were published.
    ///
    /// Rides in rot_flags for the same reason everything else here does: the
    /// stride is a contract with map_view.gd, and ADR-005 item 2 already found
    /// that depth *within* a level needs no field of its own. This is the range
    /// that item said was still missing -- it turned out to be four bits, not a
    /// wider command.
    ///
    /// Only downward. Nothing above the avatar is published: the view draws what
    /// you stand in and what you can see down into, and a ceiling drawn over it
    /// would hide the thing the view is for.
    cmd_z_below_shift = 9,
    cmd_z_below_mask = 0xF << cmd_z_below_shift,
    /// Bits 13-15: what a standing tile *is*, coarsely (3D-8c). The renderer's
    /// extrusion can fit a box to a sprite's paint, but paint cannot say that a
    /// closed door is thin or that a curtained window still seals its wall run
    /// -- only the game data knows, and these three bits are it saying so.
    /// Values are @ref cmd_shape; zero claims nothing and leaves the renderer
    /// to its own fitting.
    cmd_shape_shift = 13,
    cmd_shape_mask = 0x7 << cmd_shape_shift,
    /// Bits 16-30: *which* terrain or furniture this command draws, as one plus
    /// an index into the interned id table (3D-8d) -- zero claims nothing. The
    /// shape bits above say what kind of thing a tile is; these say which thing,
    /// which is what lets a renderer swap a sprite for a mesh it has for that
    /// id. The table is append-only for the life of the session, so an index a
    /// consumer cached last frame still names the same id this frame; see
    /// @ref MapSnapshot::copy_ident_table.
    cmd_ident_shift = 16,
    cmd_ident_mask = 0x7FFF << cmd_ident_shift,
};

/// What kind of standing thing a draw command's tile is; rides in the
/// @ref cmd_shape_shift bits of rot_flags. Coarse on purpose: the renderer
/// needs depths, not a terrain catalogue -- anything finer belongs in a mesh
/// library keyed by id, not in three bits.
enum class cmd_shape : int32_t {
    /// No claim; the renderer fits the paint.
    none = 0,
    /// Full footprint, a tile deep: wall runs must seal.
    wall = 1,
    /// As deep as a wall, because a window is a wall with glass in it.
    window = 2,
    /// Thin, at the tile's face: a door is a panel, not a metre of oak.
    door = 3,
    /// Thin: fences, railings, bars.
    thin = 4,
};

/// Deepest level below the avatar a draw command can name; see
/// @ref cmd_z_below_shift. The game's own limit is fov_3d_z_range (10), so the
/// four bits are not the binding constraint -- the first floor underfoot is.
constexpr int max_z_below = 15;

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
 * One creature in view, published so the renderer can know *what* it is drawing
 * (ADR-006's mesh amendment, item 3D-7c).
 *
 * The draw list deliberately carries no identity: a `map_draw_cmd` is an atlas
 * sub-rect and a destination, which is everything a sprite needs and nothing a mesh
 * can use. Choosing a model for a zombie requires knowing it is a zombie, so the
 * identity travels on its own channel rather than being smuggled into the command.
 *
 * A handful of entries per frame -- one per visible creature -- so this is an Array of
 * Dictionaries rather than a packed int array. The same reasoning as
 * @ref MapSnapshot::copy_sprite_coverage: the data is small, strings are the point of
 * it, and packing would cost a decode on the far side for nothing.
 */
struct creature_record {
    /// What the mesh registry looks up: a monster's type id, or the body sprite id a
    /// character draws with ("player_male", "npc_female").
    std::string id;
    /// 0 monster, 1 NPC, 2 the avatar. Enough to scale a stand-in differently, and to
    /// tell the one creature that is the viewpoint from the rest.
    int32_t kind = 0;
    /// Which creature this is, frame to frame; see @ref creature_uid. Two records at
    /// the same tile in consecutive frames could be one creature or two that swapped
    /// places, and an animated mesh must not restart its walk cycle -- or play a hit
    /// on the wrong body -- because of the ambiguity.
    int32_t uid = 0;
    /// Gait, for the mesh's locomotion animation: 0 walk, 1 run, 2 crouch, 3 prone.
    /// Monsters are always 0 -- the game gives them no move mode to read.
    int32_t move_mode = 0;
    /// View-relative pixels of the creature's **feet**: the bottom centre of its tile.
    /// The same space `map_draw_cmd::dest_x` is in, so a renderer needs no second
    /// mapping, and the same anchor the contact shadows and proxies use.
    int32_t x = 0;
    int32_t y = 0;
    /// Levels below the avatar's; see @ref cmd_z_below_shift.
    int32_t z_below = 0;
    /// Facing left, which SDL draws by mirroring rather than by rotating.
    bool flip = false;
};

/**
 * A stable-enough identity for @p critter, for @ref creature_record::uid and the
 * hit channel's attacker/target fields. Game thread only: the serial map behind
 * it is unlocked, and update_from_game is what ages it.
 *
 * Characters have a real identity -- character_id, positive, stable across saves
 * -- so they simply use it. Monsters have none: the game finds them by position,
 * and their only per-instance handle is the object's address, which the allocator
 * hands to a new monster the moment an old one dies. So monsters get negative
 * serials minted on first sight, keyed by pointer, and forgotten a few frames
 * after the pointer stops appearing -- see the prune in update_from_game for why
 * that window is what makes a recycled pointer read as a new creature.
 */
int32_t creature_uid( const Creature &critter );

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
                                           const std::string &category,
                                           const std::string &variant = {} ) const;

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

        /**
         * Every creature in view, by identity (@ref creature_record).
         *
         * Each entry is { id, kind, uid, move_mode, x, y, z_below, flip }. Published
         * every frame beside the draw list, from the same walk, so a consumer that
         * matches a creature to its sprite by tile is matching within one frame's
         * state.
         */
        godot::Array copy_creatures() const;

        /// Bumped by every @ref update_from_game. The Godot side polls this so it
        /// can skip copying and re-batching an unchanged draw list -- MapView
        /// refreshes per frame but the map only changes per turn.
        uint64_t generation() const;

        /// The interned id table for @ref cmd_ident_shift: element N is the base
        /// terrain/furniture id whose commands carry N + 1. Append-only, so a
        /// consumer can cache by index and re-copy only when the size grows.
        godot::PackedStringArray copy_ident_table() const;

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
        /// Creatures in view this frame; see @ref creature_record.
        std::vector<creature_record> creatures_;

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
        /// The interned id table (3D-8d); see @ref copy_ident_table. The table
        /// itself is shared with the Godot thread and appends under mutex_; the
        /// index beside it is the game thread's private lookup and takes no
        /// lock, because @ref update_from_game is the only writer and reader.
        std::vector<std::string> ident_table_;
        std::unordered_map<std::string, int32_t> ident_index_;
        /// Commands emitted at each @ref sprite_fallback level, this frame.
        std::array<int32_t, static_cast<size_t>( sprite_fallback::last )> fallback_counts_{};
        /// Commands emitted on each @ref map_layer, this frame.
        std::array<int32_t, 16> layer_counts_{};
        /// Sprites drawn ducked out of the player's way this frame, and of
        /// those, how many swapped to a "_transparent" variant. A consumption
        /// signal: the data has been parsed since the port began and a counter
        /// stuck at zero is how "never runs" hides.
        int32_t retracted_count_ = 0;
        int32_t transparent_count_ = 0;
        /// Tall sprites dimmed because they were standing in front of the
        /// avatar. The fallback for a tileset that cannot retract, which is
        /// every tileset the port has run against so far.
        int32_t faded_count_ = 0;
        /// Tall sprites the fade was *considered* for. Reported alongside
        /// faded_count_ because zero faded means nothing on its own: zero of
        /// zero is a scene with nothing tall in it, zero of many is a bug.
        int32_t tall_candidates_ = 0;
        /// What the z-level walk cost this frame (ADR-005 item 1).
        ///
        /// The ADR called item 1 the expensive one on the grounds that per-tile
        /// work multiplies by the number of levels drawn. These three numbers
        /// are that multiplier, measured rather than assumed -- which is the
        /// habit the same ADR named after three features in a row that were
        /// reasoned about at length and settled by printing a value.
        ///
        /// `open_columns_` is the count of view columns whose tile had no floor,
        /// so the walk descended; on an outdoor level it is zero and the whole
        /// feature costs one floor_cache lookup per tile.
        int32_t open_columns_ = 0;
        /// Deepest level below the avatar any command reached, 0 for a flat view.
        int32_t deepest_z_below_ = 0;
        /// Commands emitted for levels below the avatar's.
        int32_t below_cmds_ = 0;
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

/**
 * Whether the view's centre has moved since the last published frame.
 *
 * The centre is pos + view_offset, and view_offset is what look-around pans --
 * the game thread parks in look_around's own input loop, where nothing
 * republishes per cursor move the way do_turn republishes per turn. The input
 * wait polls this (game thread only) and republishes when it answers true,
 * which is what makes the camera follow the look cursor at all.
 */
bool view_center_moved();

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_MAP_SNAPSHOT_H
