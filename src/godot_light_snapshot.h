#pragma once
#ifndef CATA_SRC_GODOT_LIGHT_SNAPSHOT_H
#define CATA_SRC_GODOT_LIGHT_SNAPSHOT_H

#if defined(GODOT)

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "lightmap.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>

namespace godot_backend
{

/**
 * Per-tile lighting, published as a texture rather than baked into each sprite
 * (SP-3, SP-4; ADR-003).
 *
 * The renderer's lighting was a flat per-sprite tint: cata_tiles picks one of
 * five pre-filtered atlases, MapSnapshot picks one of five colours and
 * multiplies it in. Both make light a property of the sprite, so a lantern
 * lights *tiles* -- a staircase of hard 32-pixel steps -- and a remembered tile
 * is recognisable only because someone chose a colour for it that nothing else
 * uses.
 *
 * One texel per published map tile fixes both. The tile shader samples it
 * twice, from two samplers over the same texture:
 *
 *   - R, visibility, sampled **nearest**. 255 seen, 64 remembered, 0 never
 *     seen. Memory is a per-tile fact and must not bleed across a boundary: a
 *     remembered tile beside a seen one would smear into it.
 *   - G, light amount, sampled **linear**. This is the whole point of the
 *     texture: interpolating between tile centres turns the staircase into a
 *     gradient at no cost, because the hardware does it.
 *   - B is reserved for the fire flicker mask (SP-6), A is unused.
 *
 * Written by the game thread inside MapSnapshot::update_from_game, so the
 * extent and origin always match the draw list published with it. Read by the
 * Godot thread as an Image.
 */
class LightSnapshot
{
    public:
        /// Bytes per texel in the published image (RGBA8).
        static constexpr int channels = 4;

        /**
         * Floats per light in @ref copy_lights: x, y, radius, r, g, b, luminance,
         * bearing, cone.
         *
         * Floats rather than the packed ints every other channel uses, because nothing
         * here is a pixel rect or a bitfield -- a radius, a colour and an angle want to
         * be numbers, and packing them would cost a decode on the far side for nothing.
         *
         * `cone` is the full width of the beam in degrees, or 0 for a light that shines
         * everywhere. That is the whole difference between a lamp and a headlight, and
         * it is why both travel on one channel: `map::apply_light_arc` already carries a
         * bearing and a width, so a spotlight is a point light that admits to having a
         * direction.
         */
        static constexpr int light_stride = 9;

        /// Visibility values written to the R channel. Chosen so the shader can
        /// separate them with one smoothstep and no magic numbers of its own.
        static constexpr uint8_t vis_seen = 255;
        static constexpr uint8_t vis_remembered = 64;
        static constexpr uint8_t vis_unknown = 0;

        /**
         * Levels the texture has room for: the avatar's, plus every one that can be
         * published below it (@ref max_z_below + 1, the range the draw command's four
         * flag bits hold).
         *
         * The published image is one block of @p h rows per level, so a lower level's
         * light is its own rather than the column's above it. It was the column's until
         * 3D-4: one texel per column meant a basement had to sit out the light pass
         * entirely, because the texel over it belonged to the tile the avatar could see
         * and applying it a storey down would light a cellar with the daylight falling on
         * the roof. Only the levels actually reached are published.
         */
        static constexpr int max_levels = 16;

        /// Start a frame of @p w x @p h tiles, with room for every level. Clears to
        /// "never seen".
        void begin( int w, int h );
        /// Set one tile on level @p level below the avatar's. Out-of-range is ignored.
        void set( int x, int y, int level, uint8_t visibility, uint8_t light );
        /// Set the fire mask (B) for one tile, 0-255 (SP-6).
        void set_fire( int x, int y, int level, uint8_t fire );
        /// Spread the fire mask one tile in each direction before publishing.
        ///
        /// The B channel is sampled bilinearly like the light, but a fire
        /// occupies one tile: without this its glow would stop dead at the tile
        /// edge, which is exactly the hard-edged look the texture exists to get
        /// rid of. One box blur is enough to make the falloff read as a glow.
        void blur_fire();
        /**
         * Add one light source the game is casting (ADR-006 item 3D-2).
         *
         * **Read, not derived.** `map::generate_lightmap` already fills
         * `level_cache::light_source_buffer` every turn from terrain, furniture and
         * field `light_emitted`, with the source's own colour attached; this walks
         * the view and publishes what is there. ADR-005's lesson applied before the
         * fact rather than after it -- the estimate for this said "derive discrete
         * lights from the lightmap", and the lightmap's *inputs* were one struct
         * away the whole time.
         *
         * Not everything is in that buffer, and the rest is now tapped where the
         * snapshot already had the object in its hand: the light the avatar is carrying
         * (`Character::active_light`), what an NPC is carrying, what a glowing monster
         * emits (`mtype::luminance`), and vehicle headlights, which arrive with a
         * bearing and a cone because `map::apply_light_arc` has both.
         *
         * The vehicle pass mirrors the loop in `map::generate_lightmap` rather than
         * reading a result, because there is no result to read -- an arc is applied
         * straight to the lightmap and never buffered. That is a duplicated rule and
         * the one place here that can silently drift from the game; if headlights stop
         * agreeing with what the map says is lit, this is why.
         *
         * @param bearing_deg compass degrees the beam points along, 0 for a point light.
         * @param cone_deg full beam width in degrees, 0 for a light with no direction.
         *
         * @param x view-relative pixels, tile centre. The same space
         *        `map_draw_cmd::dest_x` is in, so the renderer needs no second
         *        mapping from tiles to anything.
         * @param y as @p x.
         * @param radius pixels, from `LIGHT_RANGE( luminance )` -- the game's own
         *        answer to how far this source reaches, rather than a number the
         *        renderer invents.
         * @param color the source's colour. White when it declares none, which is
         *        most of them.
         * @param luminance raw game units, deliberately unnormalised: how bright a
         *        lamp should *look* is the renderer's decision, and CDDA's scale is
         *        the only honest input to it.
         */
        void add_light( float x, float y, float radius, const light_color_rgb &color,
                        float luminance, float bearing_deg = 0.0f, float cone_deg = 0.0f );

        /// Publish the frame started by @ref begin and bump the generation.
        void commit();

        /// The published frame as an RGBA8 Image, or null when there is none.
        godot::Ref<godot::Image> copy_image() const;
        /// Tiles per level: the view's extent, not the image's height.
        godot::Vector2i size() const;
        /// Level blocks in the published image, at least one.
        int levels() const;
        uint64_t generation() const;

        /// The frame's light sources, @ref light_stride floats each.
        godot::PackedFloat32Array copy_lights() const;
        /// How many sources the published frame holds.
        int light_count() const;

        /**
         * Whether the Godot side is running the light pass.
         *
         * A handshake, not a setting. While it is false MapSnapshot keeps
         * baking light into each sprite's tint, so a host that never builds the
         * texture -- an older set of scripts, a headless run -- still gets a lit
         * map. MapView sets it once it has a texture, and from then on the tints
         * carry hue only and the texture carries brightness. Getting both at
         * once would darken everything twice.
         */
        void set_pass_enabled( bool on );
        bool pass_enabled() const;

        /**
         * Whether the renderer dims lower z-levels itself (ADR-006 item 3D-4).
         *
         * The same shape of handshake as @ref set_pass_enabled, and for the same
         * reason. `fog_for_depth` bakes a per-level dimming into every tint C++
         * publishes, which is right for a backend that draws all levels at one height
         * and wrong for one that puts them at real elevations and fades them itself --
         * both together would dim a basement twice. A host that never says so keeps
         * the baked fog, so the 2D backend is untouched.
         */
        void set_depth_fog_enabled( bool on );
        bool depth_fog_enabled() const;

        /**
         * Wind, for the sway shader (SP-7).
         *
         * It rides along here rather than being read from the weather manager
         * on the Godot thread, because the weather manager is game state and
         * the Godot thread has no business touching it. This is published in
         * the same pass as the light, which is the same rate it changes at.
         *
         * @param direction_deg compass degrees, 0 north, as the weather manager
         *        stores it.
         * @param speed_mph raw wind speed; normalised on the way out.
         */
        void set_wind( float direction_deg, float speed_mph );
        /// Screen-space wind: unit direction times 0..1 strength.
        godot::Vector2 wind() const;

        /**
         * Conditions the presentation pass grades by.
         *
         * The game already computes all of this every turn and none of it has
         * ever reached the screen: the map looks the same at midnight in a
         * downpour as at noon in clear weather, because the only thing the
         * renderer reads is per-tile light. These are the cheap inputs that
         * make time of day and weather visible.
         *
         * All normalised 0..1 so the shader needs no game constants.
         */
        struct conditions {
            /// Astronomical daylight, 0 at night and 1 at noon.
            float daylight = 1.0f;
            /// Precipitation, 0 none to 1 heavy.
            float precipitation = 0.0f;
            /// Perceived pain, saturating well before the maximum -- the point
            /// is that being hurt is visible, not that it scales linearly.
            float pain = 0.0f;
            /// Where the sun actually is, in compass degrees and degrees above the
            /// horizon, from `sun_azimuth_altitude( time_point )`.
            ///
            /// Published because the renderer was inventing it. A directional light
            /// needs a bearing, the 3D backend had a constant, and the game has known
            /// the answer all along -- `calendar.cpp` even builds the direction vector
            /// already. Altitude goes negative at night, which is the honest way to say
            /// "no sun" and is why this is not folded into @ref daylight.
            float sun_azimuth = 0.0f;
            float sun_altitude = -90.0f;
            /// What is falling, for the weather particle pass: 0 nothing, 1 rain,
            /// 2 snow, 3 acid. @ref precipitation says how much; this says what.
            /// Derived from the weather type's own `rains` / `tiles_animation`,
            /// which is the key the SDL renderer animated from -- the HUD's
            /// weather string is localized text and unusable as an id.
            int weather_kind = 0;
        };
        void set_conditions( const conditions &c );
        conditions get_conditions() const;

    private:
        /// Out-of-range marker for @ref texel.
        static constexpr size_t npos = static_cast<size_t>( -1 );
        /// Byte offset of one tile's texel on one level, or @ref npos.
        size_t texel( int x, int y, int level ) const;

        mutable std::mutex mutex_;
        /// Frame under construction. Game thread only, no lock needed.
        std::vector<uint8_t> pending_;
        int pending_w_ = 0;
        int pending_h_ = 0;
        /// Deepest level any @ref set touched this frame, so @ref commit publishes the
        /// blocks that exist rather than sixteen mostly-empty ones.
        int pending_deepest_ = 0;
        int levels_ = 1;
        std::vector<uint8_t> published_;
        /// Light sources under construction, then published; see @ref add_light.
        /// Flat, @ref light_stride floats per source, so it copies straight into a
        /// PackedFloat32Array without a per-entry conversion.
        std::vector<float> pending_lights_;
        std::vector<float> published_lights_;
        int w_ = 0;
        int h_ = 0;
        uint64_t generation_ = 0;
        std::atomic<bool> pass_enabled_{ false };
        std::atomic<bool> depth_fog_enabled_{ false };
        float wind_x_ = 0.0f;
        float wind_y_ = 0.0f;
        conditions conditions_;
};

LightSnapshot &get_light_snapshot();

/**
 * Map a tile's lighting to the 0-255 the G channel carries.
 *
 * Anchored on @p ll, the same lit_level the per-sprite tint used, rather than
 * on the raw lightmap value. Scaling the raw value linearly seems like the
 * obvious thing and is wrong: an ordinary indoor tile reads about 4 against a
 * LIGHT_AMBIENT_LIT of 10, so a room the game considers well lit came out at
 * 40% brightness -- darker than the tint it replaced, which drew the same tile
 * white.
 *
 * So the level sets the floor and @p ambient_light only moves the value within
 * that level's band. The band is what the texture's interpolation needs:
 * neighbouring tiles at different levels blend smoothly instead of stepping,
 * which is the whole point of SP-4, and the absolute brightness still matches
 * what the game says about the tile.
 */
uint8_t encode_light_level( lit_level ll, float ambient_light );

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_LIGHT_SNAPSHOT_H
