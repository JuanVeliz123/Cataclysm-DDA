#pragma once
#ifndef CATA_SRC_GODOT_DISPLAY_H
#define CATA_SRC_GODOT_DISPLAY_H

#if defined(GODOT)

#include "godot_backend.h"

namespace godot_backend
{

/**
 * Window lifecycle bridge to the Godot host.
 *
 * Godot owns the window and drives present itself, so this is only a place for
 * the game side to ask the host for a resize or a fullscreen toggle. There is no
 * frame upload: the map goes through MapSnapshot and the remaining curses screens
 * through ViewSnapshot.
 */
class GodotDisplay final : public display
{
    public:
        GodotDisplay() = default;
        ~GodotDisplay() override = default;

        bool init() override;
        void resize( int w, int h ) override;
        void toggle_fullscreen() override;
};

} // namespace godot_backend

#endif // GODOT

#endif // CATA_SRC_GODOT_DISPLAY_H
