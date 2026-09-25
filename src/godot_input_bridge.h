#pragma once
#ifndef CATA_SRC_GODOT_INPUT_BRIDGE_H
#define CATA_SRC_GODOT_INPUT_BRIDGE_H

#if defined(GODOT)

#include "input_enums.h"

#include <deque>
#include <mutex>
#include <optional>

#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/ref.hpp>

namespace godot_backend
{

/**
 * Thread-safe bridge feeding Godot input events into CDDA's input pipeline.
 *
 * The Godot host pushes @ref godot::InputEvent objects from its own thread via
 * @ref push_event; the CDDA game loop drains them with @ref pop_event after
 * they have been translated into CDDA's @ref input_event representation
 * (T3.1 in docs/godot_migration/architecture_adr.md).
 */
class GodotInputBridge
{
    public:
        GodotInputBridge() = default;
        ~GodotInputBridge() = default;

        GodotInputBridge( const GodotInputBridge & ) = delete;
        GodotInputBridge &operator=( const GodotInputBridge & ) = delete;

        /// Queue @p event for the game loop. Safe to call from any thread.
        void push_event( const godot::Ref<godot::InputEvent> &event );

        /// Pop the oldest queued event, or @ref std::nullopt if the queue is empty.
        std::optional<input_event> pop_event();

        /**
         * Describe where the cell grid sits on screen, in the same coordinate
         * space as @ref godot::InputEvent positions.
         *
         * Mouse events arrive from Godot in pixels, but because the Godot build
         * leaves TILES undefined, @ref input_context::get_coordinates takes the
         * TUI branch and reads @ref input_event::mouse_pos as *cell*
         * coordinates. Without this geometry every click resolves to the wrong
         * cell. Safe to call from any thread; the Godot host updates it whenever
         * the terminal overlay is laid out.
         */
        void set_cell_geometry( int origin_x, int origin_y, int cell_w, int cell_h );

    private:
        std::deque<input_event> queue_;
        std::mutex mutex_;
};

/// Returns the single process-lifetime input bridge.
GodotInputBridge &get_input_bridge();

} // namespace godot_backend

#endif // GODOT

#endif // CATA_SRC_GODOT_INPUT_BRIDGE_H
