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

        /// Visibility values written to the R channel. Chosen so the shader can
        /// separate them with one smoothstep and no magic numbers of its own.
        static constexpr uint8_t vis_seen = 255;
        static constexpr uint8_t vis_remembered = 64;
        static constexpr uint8_t vis_unknown = 0;

        /// Start a frame of @p w x @p h tiles. Clears to "never seen".
        void begin( int w, int h );
        /// Set one tile. Out-of-range coordinates are ignored.
        void set( int x, int y, uint8_t visibility, uint8_t light );
        /// Set the fire mask (B) for one tile, 0-255 (SP-6).
        void set_fire( int x, int y, uint8_t fire );
        /// Spread the fire mask one tile in each direction before publishing.
        ///
        /// The B channel is sampled bilinearly like the light, but a fire
        /// occupies one tile: without this its glow would stop dead at the tile
        /// edge, which is exactly the hard-edged look the texture exists to get
        /// rid of. One box blur is enough to make the falloff read as a glow.
        void blur_fire();
        /// Publish the frame started by @ref begin and bump the generation.
        void commit();

        /// The published frame as an RGBA8 Image, or null when there is none.
        godot::Ref<godot::Image> copy_image() const;
        godot::Vector2i size() const;
        uint64_t generation() const;

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

    private:
        mutable std::mutex mutex_;
        /// Frame under construction. Game thread only, no lock needed.
        std::vector<uint8_t> pending_;
        int pending_w_ = 0;
        int pending_h_ = 0;
        std::vector<uint8_t> published_;
        int w_ = 0;
        int h_ = 0;
        uint64_t generation_ = 0;
        std::atomic<bool> pass_enabled_{ false };
        float wind_x_ = 0.0f;
        float wind_y_ = 0.0f;
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
