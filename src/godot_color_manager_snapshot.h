#pragma once
#ifndef CATA_SRC_GODOT_COLOR_MANAGER_SNAPSHOT_H
#define CATA_SRC_GODOT_COLOR_MANAGER_SNAPSHOT_H

#if defined(GODOT)

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <godot_cpp/variant/dictionary.hpp>

namespace godot_backend
{

/**
 * The color manager (MENU-14, `color_manager::show_gui()` in `color.cpp`) as
 * a Godot Control.
 *
 * No tabs, but each row has two independently-editable cells (Normal /
 * Invert), so a click is addressed as (row, column) the way the legacy loop's
 * `iCurrentLine`/`iCurrentCol` cursor addressed it -- encoded into one
 * request int as `row * 2 + (col == 2 ? 1 : 0)` since there is no atomic pair.
 *
 * Picking a custom color and loading a template/theme all open a nested
 * `uilist` -- already a Godot panel, but (same move as `AutoNoteSnapshot`'s
 * `GODOT_SYMBOL`) this channel must be suspended around each call or the
 * hidden color manager keeps answering the door for the picker on top of it.
 *
 * Nothing here writes to the real color config until the legacy epilogue's
 * "Save changes?" prompt -- this channel only ever touches the same
 * in-memory `gui_name_color_map` the ImGui loop did.
 */
class ColorManagerSnapshot
{
    public:
        bool active() const;
        uint64_t generation() const;

        // --- Godot thread -------------------------------------------------
        godot::Dictionary copy_state() const;
        void request_action( const std::string &action );
        /// A clicked color cell, encoded as row * 2 + (col == 2 ? 1 : 0).
        void request_pick( int encoded );
        /// A clicked "remove custom" button, same encoding as request_pick.
        void request_remove( int encoded );
        void note_attended();
        bool attended() const;

        // --- game thread ---------------------------------------------------
        struct cell {
            std::string label;
            std::string color_name;
            bool has_custom = false;
        };
        struct row {
            std::string name;
            cell normal;
            cell invert;
        };
        struct data {
            std::string title;
            std::vector<row> rows;
        };

        void publish( const data &d );
        void set_suspended( bool suspended );
        void clear();

        /**
         * Block until the panel asks for something.
         *
         * @param row out: the row half of a pick/remove request, or -1.
         * @param col out: the column half (1 = Normal, 2 = Invert) of a
         *        pick/remove request, or -1.
         * @return "GODOT_PICK", "GODOT_REMOVE", "GODOT_TEMPLATE",
         *         "GODOT_THEME", "QUIT", or "" when no panel attended -- the
         *         caller must then run the legacy ImGui loop.
         */
        std::string next_action( int &row, int &col );

    private:
        mutable std::mutex mutex_;
        bool active_ = false;
        data data_;
        uint64_t generation_ = 0;
        std::vector<std::string> pending_actions_;
        std::atomic<int> pending_pick_{ -1 };
        std::atomic<int> pending_remove_{ -1 };
        std::atomic<bool> suspended_{ false };
        std::atomic<bool> attended_{ false };
};

ColorManagerSnapshot &get_color_manager_snapshot();

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_COLOR_MANAGER_SNAPSHOT_H
