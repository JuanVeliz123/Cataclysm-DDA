#pragma once
#ifndef CATA_SRC_GODOT_LOOK_SNAPSHOT_H
#define CATA_SRC_GODOT_LOOK_SNAPSHOT_H

#if defined(GODOT)

#include <mutex>

namespace godot_backend
{

/**
 * The identity of game::look_around()'s info window (w_info), while it
 * exists.
 *
 * look_around() already draws through the normal catacurses -> ViewSnapshot
 * pipeline (see draw_window() in godot_curses_backend.cpp), already receives
 * input correctly through the same queue every other legacy screen reads
 * from, and is already shown on screen by TerminalView, the generic overlay
 * for any legacy screen that has not been given its own Godot panel yet
 * (host.gd's USE_CURSES_UI_OVERLAY is on). It was never given a takeover loop
 * of its own, and doesn't need one -- and it does not need a dedicated panel
 * either, since TerminalView already does that job. (An earlier version of
 * this fix added one anyway; it rendered the same cells TerminalView does, so
 * the two were stacked on screen at once.)
 *
 * The one real gap was that draw_window()'s heuristic for "this must be the
 * game's own sidebar, which Godot's hud_panel draws instead" also matched
 * w_info, which is deliberately drawn at that same rect -- so its content was
 * erased every frame before TerminalView ever saw it. This snapshot exists
 * solely so draw_window() can tell w_info apart from the real sidebar by
 * identity (a rect match is ambiguous: both windows share the same rect).
 */
class LookSnapshot
{
    public:
        bool active() const;
        /**
         * Whether @p window_id (a catacurses::window's underlying native
         * pointer, i.e. `w.get<void>()`) is w_info itself.
         */
        bool is_window( const void *window_id ) const;

        void publish( const void *window_id );
        void clear();

    private:
        mutable std::mutex mutex_;
        bool active_ = false;
        const void *window_id_ = nullptr;
};

LookSnapshot &get_look_snapshot();

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_LOOK_SNAPSHOT_H
