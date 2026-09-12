#pragma once
#ifndef CATA_SRC_GODOT_SAFEMODE_SNAPSHOT_H
#define CATA_SRC_GODOT_SAFEMODE_SNAPSHOT_H

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
 * The safe mode manager (MENU-14, `safemode::show()` in `safemode_ui.cpp`)
 * as a Godot Control.
 *
 * Two tabs (global / character), like `auto_note`'s -- `gui_tab` on the
 * game side is the authoritative tab, since row-only requests (remove,
 * copy, enable, ...) carry no tab of their own. A cell click is addressed
 * as (row, column), encoded into one request int as `row * MAX_COLUMN + col`
 * the same way `ColorManagerSnapshot` encodes its (row, column) picks.
 *
 * The rule-text and proximity-distance edit prompts are `string_input_popup_imgui`,
 * which already routes itself through the Godot text-prompt channel
 * (`godot_popup_snapshot.h`) -- no bespoke panel needed, just a suspend
 * around the call the same way `AutoNoteSnapshot`'s `GODOT_SYMBOL` suspends
 * for its nested popups. TEST_RULE's match list is a plain `uilist` for the
 * same reason (MENU-4's "drive the existing implementation" insight).
 *
 * Nothing here writes to the real safe mode config until the legacy
 * epilogue's "Save changes?" prompt -- this channel only ever touches the
 * same `global_rules` / `character_rules` vectors the ImGui loop did.
 */
class SafemodeSnapshot
{
    public:
        /// Column count `request_confirm()`'s encoding divides by. Kept in
        /// sync with `safemode::Columns::MAX_COLUMN` (safemode_ui.h) by the
        /// caller on the game-thread side -- there's no shared header
        /// between the two, so this is the one number that must match.
        static constexpr int MAX_COLUMN = 6;

        bool active() const;
        uint64_t generation() const;

        // --- Godot thread -------------------------------------------------
        godot::Dictionary copy_state() const;
        void request_action( const std::string &action );
        /// A clicked tab: 0 = global, 1 = character.
        void request_tab( int tab );
        /// A clicked cell, encoded as row * MAX_COLUMN + column.
        void request_confirm( int encoded );
        void request_remove( int row );
        void request_copy( int row );
        void request_swap( int row );
        void request_enable( int row );
        void request_disable( int row );
        void request_move_up( int row );
        void request_move_down( int row );
        void request_test( int row );
        void note_attended();
        bool attended() const;

        // --- game thread ---------------------------------------------------
        struct row {
            std::string rule;
            bool active = true;
            std::string attitude;
            std::string proximity;
            bool whitelist = false;
            std::string category;
            std::string movement_mode;
        };
        struct data {
            std::string title;
            int tab = 0;
            bool character_locked = false;
            bool safe_mode_on = false;
            bool show_swap = false;
            std::vector<row> rows;
        };

        void publish( const data &d );
        void set_suspended( bool suspended );
        void clear();

        /**
         * Block until the panel asks for something.
         *
         * @param row out: the row half of a row-addressed request, or -1.
         * @param col out: the column half of a GODOT_CONFIRM request (or the
         *        tab half of a GODOT_TAB request), or -1.
         * @return "GODOT_TAB", "GODOT_CONFIRM", "GODOT_REMOVE", "GODOT_COPY",
         *         "GODOT_SWAP", "GODOT_ENABLE", "GODOT_DISABLE",
         *         "GODOT_MOVE_UP", "GODOT_MOVE_DOWN", "GODOT_TEST",
         *         "ADD_RULE", "ADD_DEFAULT_RULESET", "QUIT", or "" when no
         *         panel attended -- the caller must then run the legacy
         *         ImGui loop.
         */
        std::string next_action( int &row, int &col );

    private:
        mutable std::mutex mutex_;
        bool active_ = false;
        data data_;
        uint64_t generation_ = 0;
        std::vector<std::string> pending_actions_;
        std::atomic<int> pending_tab_{ -1 };
        std::atomic<int> pending_confirm_{ -1 };
        std::atomic<int> pending_remove_{ -1 };
        std::atomic<int> pending_copy_{ -1 };
        std::atomic<int> pending_swap_{ -1 };
        std::atomic<int> pending_enable_{ -1 };
        std::atomic<int> pending_disable_{ -1 };
        std::atomic<int> pending_move_up_{ -1 };
        std::atomic<int> pending_move_down_{ -1 };
        std::atomic<int> pending_test_{ -1 };
        std::atomic<bool> suspended_{ false };
        std::atomic<bool> attended_{ false };
};

SafemodeSnapshot &get_safemode_snapshot();

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_SAFEMODE_SNAPSHOT_H
