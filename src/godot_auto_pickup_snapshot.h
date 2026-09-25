#pragma once
#ifndef CATA_SRC_GODOT_AUTO_PICKUP_SNAPSHOT_H
#define CATA_SRC_GODOT_AUTO_PICKUP_SNAPSHOT_H

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
 * Auto pickup manager (MENU-14, `auto_pickup::user_interface::show()` in
 * `auto_pickup.cpp`) as a Godot Control.
 *
 * Same rule-editor shape as `safemode`, but a generic one: `user_interface`
 * is shared by the player screen (1 or 2 tabs -- global, plus character only
 * once a character is loaded, unlike safemode's always-two-but-locked tab)
 * and the NPC pickup-rules screen (exactly 1 tab, titled with the NPC's own
 * name). So this snapshot carries a list of tab titles rather than assuming
 * "global/character", and publishes only the current tab's rows -- the same
 * "tab switch refetches" shape `mission_ui` and `safemode` use.
 *
 * Rules here have just two columns (rule text, include/exclude), not
 * safemode's six -- a cell click is encoded as `row * MAX_COLUMN + col` the
 * same way, just with `MAX_COLUMN == 2`.
 *
 * The rule-text edit prompt is `string_input_popup_imgui`, which already
 * routes itself through the Godot text-prompt channel (`godot_popup_snapshot.h`)
 * -- no bespoke panel needed, just a suspend around the call the same way
 * `SafemodeSnapshot`'s GODOT_CONFIRM does. `rule::test_pattern()` was
 * changed to build a plain `uilist` (already a migrated Godot panel) instead
 * of its own bespoke scrollable window, the same MENU-4 insight safemode's
 * TEST_RULE applied a second time -- so GODOT_TEST only needs to suspend
 * around that call, not drive a channel of its own.
 *
 * ADD_RULE here just appends a blank rule rather than immediately opening
 * the text prompt the legacy loop does -- the same simplified
 * re-presentation `safemode`'s own ADD_RULE settled on, so a click on the
 * new row's rule cell edits it instead.
 *
 * Nothing here writes to the real auto-pickup config until the legacy
 * epilogue's "Save changes?" prompt -- this channel only ever touches the
 * same `tabs[].new_rules` vectors the ImGui loop did.
 */
class AutoPickupSnapshot
{
    public:
        /// Column count `request_confirm()`'s encoding divides by.
        static constexpr int MAX_COLUMN = 2;

        bool active() const;
        uint64_t generation() const;

        // --- Godot thread -------------------------------------------------
        godot::Dictionary copy_state() const;
        void request_action( const std::string &action );
        /// A clicked tab, by index into the published `tab_titles`.
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
            bool exclude = false;
        };
        struct data {
            std::string title;
            int tab = 0;
            std::vector<std::string> tab_titles;
            /// Whether a row can be swapped to the other tab -- only when
            /// there are exactly two, the same `allow_swapping` rule the
            /// legacy loop uses.
            bool show_swap = false;
            bool auto_pickup_on = false;
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
         *         "ADD_RULE", "SWITCH_AUTO_PICKUP_OPTION", "QUIT", or "" when
         *         no panel attended -- the caller must then run the legacy
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

AutoPickupSnapshot &get_auto_pickup_snapshot();

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_AUTO_PICKUP_SNAPSHOT_H
