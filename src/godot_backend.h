#pragma once
#ifndef CATA_SRC_GODOT_BACKEND_H
#define CATA_SRC_GODOT_BACKEND_H

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "point.h"
#include "color.h"
#include "lightmap.h"
#include "cursesdef.h"

#if defined(GODOT)
#include <godot_cpp/classes/image_texture.hpp>
#endif

/**
 * Shared contract for the Godot rendering backend.
 *
 * Architecture (see docs/godot_migration/architecture_adr.md, ADR-002):
 *  - Target: C++ owns game logic + catacurses cell buffers; Godot owns present
 *    (TerminalView draws a cell snapshot via TextServer).
 *  - The in-session map is drawn by MapView from a packed draw list (ADR-003);
 *    the CPU raster path this header used to define is gone.
 *
 * This header must NOT depend on SDL headers. It is the interface contract
 * implemented by the modules in src/godot_*.{h,cpp}.
 */

namespace godot_backend
{

/// RGBA color, layout-compatible with a 32-bit pixel.
struct color {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 255;
};

/// The 16-color game palette (mirror of the SDL windowsPalette).
using palette_array = std::array<color, 16>;


/// Converts a game color (@ref nc_color) to an RGB @ref color using the active
/// palette. Analog of the SDL backend's curses_color_to_SDL (sdl_utils.cpp).
color curses_color_to_color( const nc_color &color );



/**
 * Window/terminal lifecycle bridge to the Godot host.
 *
 * The host project (godot/) owns the window. The game side calls @ref init once,
 * and resize/fullscreen on demand; Godot drives present itself.
 */
class display
{
    public:
        virtual ~display() = default;

        /// Initialize the bridge (attach to the Godot host surface). Returns
        /// false on failure.
        virtual bool init() = 0;

        /// Notify the host of a requested terminal resize (in pixels).
        virtual void resize( int w, int h ) = 0;

        /// Toggle fullscreen on the host window.
        virtual void toggle_fullscreen() = 0;
};
using display_ptr = std::unique_ptr<display>;

/// Returns the single process-lifetime display bridge (created in init_interface).
display *get_display();

/// Queue a host window size in pixels; applied on the game thread.
void request_window_resize( int pixel_w, int pixel_h );
/// Drain pending host resize and recreate the cell grid.
void apply_pending_window_resize();

/// Signal process shutdown (window close / Quit). Wakes input waits.
void request_shutdown();
bool is_shutdown_requested();

/**
 * Drop every godot::Ref the backend holds in a file-static.
 *
 * Must be called from the Godot main thread while the engine is still up.
 * A Ref released during static destruction unrefs an Object after Godot has
 * already torn down its ObjectDB, which faults on freed memory -- intermittently,
 * with whatever signal the reused allocation happens to produce, and always
 * after the run's real work is finished. The two tilesets hold dozens of these
 * between them.
 */
void release_godot_resources();

/// Composite the ImTui cell screen (ImGui popups) into the cell snapshot.
void blit_imtui_screen();

// --- Concrete module factories -------------------------------------------
// Each factory is implemented by a specific Phase 1 module. Declaring them in
// this shared header lets the modules be developed in parallel against one
// contract and wired together during reconciliation.

/// Implemented by src/godot_display.{h,cpp} (T1.4). Creates and stores the
/// process-lifetime display bridge, returning a non-owning pointer (same as
/// @ref get_display).
display *create_display();

#if defined(GODOT)

/**
 * GPU sprite texture for the Godot backend.
 *
 * Wraps a shared atlas @ref godot::ImageTexture plus the sub-rect of a single
 * sprite inside it, plus the tightest opaque (non-transparent) bounds of that
 * sprite. Mirrors the SDL backend's @ref texture class (src/cata_tiles.h) so
 * the tileset loader and the draw path share one sprite representation. The
 * atlas texture is kept alive by the @ref godot::Ref it holds; the tileset
 * keeps an owning ref to the atlas as well (godot_tileset::atlas_textures).
 */
class godot_texture
{
    public:
        godot_texture() = default;
        /// @param tex the atlas texture the sprite lives in
        /// @param x,y,w,h source rect of the sprite within the atlas
        godot_texture( godot::Ref<godot::ImageTexture> tex, int x, int y, int w, int h )
            : texture_( std::move( tex ) ), x_( x ), y_( y ), width_( w ), height_( h ),
              opaque_x_( x ), opaque_y_( y ), opaque_width_( w ), opaque_height_( h ) {}
        /// As above, but with explicit opaque bounds relative to the sprite
        /// origin (used for tint overlays so transparent padding is excluded).
        godot_texture( godot::Ref<godot::ImageTexture> tex, int x, int y, int w, int h,
                       int opaque_x, int opaque_y, int opaque_w, int opaque_h )
            : texture_( std::move( tex ) ), x_( x ), y_( y ), width_( w ), height_( h ),
              opaque_x_( opaque_x ), opaque_y_( opaque_y ),
              opaque_width_( opaque_w ), opaque_height_( opaque_h ) {}

        bool is_valid() const {
            return texture_.is_valid();
        }
        /// The width (first) and height (second) of the sprite.
        std::pair<int, int> dimension() const {
            return { width_, height_ };
        }
        /// The underlying atlas texture (shared by every sprite of the atlas).
        const godot::Ref<godot::ImageTexture> &get_texture() const {
            return texture_;
        }
        int src_x() const {
            return x_;
        }
        int src_y() const {
            return y_;
        }
        int src_w() const {
            return width_;
        }
        int src_h() const {
            return height_;
        }
        /// The opaque pixel bounding box relative to the sprite origin.
        int opaque_x() const {
            return opaque_x_;
        }
        int opaque_y() const {
            return opaque_y_;
        }
        int opaque_w() const {
            return opaque_width_;
        }
        int opaque_h() const {
            return opaque_height_;
        }

    private:
        godot::Ref<godot::ImageTexture> texture_;
        int x_ = 0;
        int y_ = 0;
        int width_ = 0;
        int height_ = 0;
        int opaque_x_ = 0;
        int opaque_y_ = 0;
        int opaque_width_ = 0;
        int opaque_height_ = 0;
};

#endif // GODOT

} // namespace godot_backend

#endif // CATA_SRC_GODOT_BACKEND_H
