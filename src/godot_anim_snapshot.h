#pragma once
#ifndef CATA_SRC_GODOT_ANIM_SNAPSHOT_H
#define CATA_SRC_GODOT_ANIM_SNAPSHOT_H

#if defined(GODOT)

#include <cstdint>
#include <mutex>
#include <vector>

#include <string>

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>

#include "coordinates.h"

class Creature;
class nc_color;

namespace godot_backend
{

enum class anim_kind : int32_t {
    /// A character drawn over a map tile: explosion rings, bullets, hit markers,
    /// the aim cursor.
    glyph = 0,
    /// A tile-sized highlight, for the tiles along a trajectory or aim line.
    highlight = 1,
};

/// One animation overlay primitive, in bubble map coordinates.
/// Packed as 7 ints: kind, x, y, z, codepoint, fg, bg.
struct anim_cmd {
    int32_t kind = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    int32_t codepoint = 0;
    /// 0xRRGGBBAA, same convention as map_draw_cmd::tint.
    int32_t fg = 0;
    /// 0xRRGGBBAA, or 0 for no background.
    int32_t bg = 0;
};

/// Where a run of text sits relative to its anchor tile. Mirrors the
/// direction-to-alignment mapping in formatted_text's constructor
/// (src/cata_tiles.cpp), so combat text drifts the same way it does under SDL.
enum class anim_align : int32_t {
    left = 0,
    center = 1,
    right = 2,
};

/**
 * A run of scrolling combat text: damage numbers, "Critical!", healing, XP.
 *
 * These never reached the player in this build. game::draw_sct() had a TILES
 * branch and an #else, and the #else writes into w_terrain -- which the Godot
 * backend deliberately skips, because MapView owns the map. So the numbers were
 * being computed, positioned and stepped every turn, and thrown away.
 *
 * Unlike @ref anim_cmd this carries a string, so it cannot ride in the packed
 * int array and is published as its own list.
 */
struct anim_text {
    /// Anchor, in bubble map coordinates. cSCT::getPosX/getPosY already include
    /// the scroll offset for the current step, so this moves on its own as the
    /// text rises.
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    std::string text;
    /// 0xRRGGBBAA, same convention as anim_cmd::fg.
    int32_t fg = 0;
    anim_align align = anim_align::center;
    /// 1.0 when fresh, falling to 0.0 as the text ages out, for the fade.
    float life = 1.0f;
    /// Which half of the message this is: 0 the amount, 1 the qualifier
    /// ("17", then "Critical!"). They share an anchor and are drawn adjacent,
    /// which is what the curses path does by printing one after the other.
    /// cata_tiles emplaces both at the same point and lets them overlap; that
    /// is not worth reproducing.
    int32_t run = 0;
};

/**
 * One creature taking a hit (SP-5).
 *
 * Creatures are inside the batched draw list, as ordinary tiles on the monster
 * and player layers, so nothing about them can tween on its own. But a
 * MultiMesh instance carries its own transform: a lunge is a per-instance
 * offset, and a flash is a per-instance colour. Neither needs the creature
 * lifted out of the batch, and neither needs a frame of baked art.
 *
 * So this publishes the *event* and lets MapView own the timing. The game
 * thread has no business knowing how long a recoil lasts, and the alternative
 * -- driving it from here -- would tie the animation to the turn rate.
 *
 * Packed as 10 ints: id, x, y, z, dir_x, dir_y, flash, attacker_uid,
 * target_uid, kind.
 */
struct anim_hit {
    /// Monotonic, so MapView can tell a new hit from one it has already played
    /// without the two sides sharing a clock.
    int32_t id = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    /// Which way the blow came from, in tiles, -1..1 on each axis. Zero when
    /// there is nobody to point at, which MapView reads as a straight recoil.
    int32_t dir_x = 0;
    int32_t dir_y = 0;
    /// 0xRRGGBBAA to flash toward.
    int32_t flash = 0;
    /// Who struck and who was struck; see creature_uid (godot_map_snapshot.h).
    /// A tile match was enough to nudge a sprite, but an animated mesh has to
    /// play its reaction on a specific body, and the tile under a melee is
    /// exactly where two creatures are most likely to stand adjacent. Zero
    /// means unknown, which the sprite path already tolerates.
    int32_t attacker_uid = 0;
    int32_t target_uid = 0;
    /// 0 a hit, 1 a death, 2 a swing. One channel for all three because they
    /// share everything else: the polling, the aging, the "do not miss one
    /// between polls".
    int32_t kind = 0;
};

/**
 * Overlay primitives for the in-progress animation frame.
 *
 * CDDA animates by registering a game::draw_callback_t and then re-running
 * ui_manager::redraw() once per animation frame. Under the SDL tiles build those
 * callbacks poke cata_tiles; under curses they write glyphs into `w_terrain`,
 * which the Godot backend deliberately skips because MapView owns the map. So the
 * GODOT branches in animation.cpp publish the same information here instead,
 * positioned in map coordinates rather than terrain-window cells, and the Godot
 * side draws it over MapView.
 *
 * Frames are committed rather than cleared by each callback: several callbacks can
 * be registered at once (an explosion and a cursor, say), so they all append to a
 * pending list which is published as one frame when `w_terrain` is refreshed --
 * that is, after game::draw has run every callback.
 */
class AnimSnapshot
{
    public:
        /// Ints per packed command in @ref copy_commands.
        static constexpr int cmd_stride = 7;

        /// Ints per packed hit in @ref copy_hits.
        static constexpr int hit_stride = 10;

        void add_glyph( const tripoint_bub_ms &p, char32_t ch, const nc_color &color );
        void add_highlight( const tripoint_bub_ms &p );
        /// Append a run of combat text to the frame being built.
        void add_text( const tripoint_bub_ms &p, const std::string &text,
                       const nc_color &color, anim_align align, float life, int run );

        /// The current frame's combat text, as Dictionaries. Not packed ints:
        /// these carry strings.
        godot::Array copy_texts() const;

        /// Record a hit at @p p, struck from @p from. Unlike the glyph
        /// primitives this is not part of a frame: it is kept until it ages out
        /// of the recent list, because MapView polls at its own rate and must
        /// not miss one that landed between polls. The uids default to zero --
        /// unknown -- so a caller that has no identity to offer says nothing
        /// rather than something wrong.
        void add_hit( const tripoint_bub_ms &p, const tripoint_bub_ms &from,
                      const nc_color &color, int32_t attacker_uid = 0,
                      int32_t target_uid = 0, int32_t kind = 0 );
        /// Record a death at @p p: an anim_hit with kind 1, no direction and no
        /// flash colour -- what a death looks like is the mesh's business, not a
        /// tint the game picked for a blow.
        void add_death( const tripoint_bub_ms &p, int32_t target_uid );
        /// Record a swing at @p attacker_pos: an anim_hit with kind 2, no
        /// direction and no flash. A swing exists for the attacker's animation
        /// only -- the sprite lunge and the flinch belong to hits -- which is
        /// why direction and flash stay zero, and why a swing fires whether or
        /// not the blow lands.
        void add_swing( const tripoint_bub_ms &attacker_pos, int32_t attacker_uid,
                        int32_t target_uid );
        godot::PackedInt32Array copy_hits() const;
        /// Id of the most recent hit. MapView compares against the last it
        /// played; equal means there is nothing new to copy.
        uint64_t hit_generation() const;

        /// Publish everything appended since the last commit as the current frame.
        /// Called on the game thread when the terrain window is refreshed.
        void commit_frame();

        godot::PackedInt32Array copy_commands() const;
        int command_count() const;

        /// Bumped by every @ref commit_frame that changes the published frame, so
        /// the Godot side can skip redrawing an unchanged (usually empty) overlay.
        uint64_t generation() const;

        /**
         * Counters for "why is nothing animating".
         *
         * The overlay has three links -- a draw callback runs, it appends a
         * primitive, a frame boundary publishes it -- and when nothing appears
         * on screen the generation alone cannot say which one failed. Silence
         * looks the same whether the callbacks never ran or ran with nothing to
         * say. These separate the two.
         *
         * @return { commits, glyphs_added, texts_added, hits_added, generation }.
         */
        godot::Dictionary copy_stats() const;

    private:
        /// Stamp @p hit with the next id and append it to the recent list,
        /// dropping the oldest past the cap. The one place hits enter, so the
        /// sequence and the bound cannot disagree between add_hit and add_death.
        void push_hit( anim_hit hit );

        mutable std::mutex mutex_;
        /// Appended by draw callbacks during game::draw. Game thread only.
        std::vector<anim_cmd> pending_;
        std::vector<anim_cmd> published_;
        std::vector<anim_text> pending_texts_;
        std::vector<anim_text> published_texts_;
        uint64_t generation_ = 0;
        /// Recent hits, oldest first. Bounded: a fight can land more blows in a
        /// turn than MapView will ever animate, and the ones it drops are the
        /// old ones.
        std::vector<anim_hit> hits_;
        uint64_t hit_seq_ = 0;
        /// Diagnostics; see @ref copy_stats.
        uint64_t commits_ = 0;
        uint64_t glyphs_added_ = 0;
        uint64_t texts_added_ = 0;
};

AnimSnapshot &get_anim_snapshot();

/**
 * Publish @p critter's death on the hit channel (anim_hit kind 1), so its mesh
 * can play a death instead of vanishing between two frames.
 *
 * Lives here rather than in godot_map_snapshot because it produces an anim_hit;
 * the identity it stamps on it (creature_uid) is borrowed from over there.
 *
 * Deliberately safe to call from anywhere, any time: die() runs during world
 * teardown and from odd corners of the game loop, so the "is there even a game"
 * guard is inside this helper, not the call sites' problem. Call it from the
 * top of a die() override and forget about it.
 */
void note_creature_death( const Creature &critter );

/**
 * Publish @p attacker swinging at @p target on the hit channel (anim_hit kind
 * 2), so the attacker's mesh can play its attack clip. Hits only name the
 * attacker when the avatar strikes a monster, which left every other swing --
 * a monster's above all -- invisible to the renderer.
 *
 * Same shape as note_creature_death, for the same reason: the "is there even a
 * game" guard is inside, and the uids are resolved here, because melee entry
 * points should not need to know about uids or channels, exactly as die() does
 * not.
 */
void note_creature_swing( const Creature &attacker, const Creature &target );

/**
 * Run the game's draw callbacks and publish the frame they produced.
 *
 * This is the Godot build's replacement for game::draw, and deliberately not a
 * repair of it. That function is eight statements, six of which are curses --
 * werase and draw_ter on w_terrain, the async and blink curses passes,
 * wnoutrefresh, and draw_panels for the sidebar. MapView owns the map and a
 * Godot panel owns the sidebar, so all six are things this port is deleting.
 * Calling game::draw to reach the other two would resurrect the curses draw
 * path to get at callback iteration, and would leave the animation overlay's
 * frame boundary hostage to a curses window's refresh.
 *
 * The two statements that matter are here instead, with no curses in reach:
 * run the callbacks, commit what they published. Callbacks are how the game
 * declares transient visual state -- an explosion, an aim line, a targeting
 * cursor, combat text -- which is engine-agnostic; only the windows they used
 * to write into were not.
 *
 * Game thread only. Cheap when nothing is animating: with no callbacks
 * registered and nothing published, commit_frame returns without bumping the
 * generation and the Godot side keeps skipping the overlay.
 *
 * Note for whoever removes the curses overlay (MENU-9): this must not be built
 * on ui_manager. The animation loops currently reach game::draw through
 * invalidate_main_ui_adaptor plus redraw_invalidated, and that stack goes away
 * with the overlay. A direct call from the game thread survives it.
 */
void publish_transient_visuals();

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_ANIM_SNAPSHOT_H
