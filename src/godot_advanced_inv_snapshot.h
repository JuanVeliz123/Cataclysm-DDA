#pragma once
#ifndef CATA_SRC_GODOT_ADVANCED_INV_SNAPSHOT_H
#define CATA_SRC_GODOT_ADVANCED_INV_SNAPSHOT_H

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
 * The advanced inventory manager ("AIM", MENU-8) as a Godot Control.
 *
 * Unlike the MENU-13 screens, both panes are on screen simultaneously (there
 * is no tab -- LEFT/RIGHT/TOGGLE_TAB just move which one is `src`), so both
 * are published every time, and a row click carries which side it landed on
 * rather than an index alone.
 *
 * advanced_inventory::process_action() is a real, already-existing state
 * machine driven by the same action strings its own input_context produces
 * ("MOVE_SINGLE_ITEM", "SORT", "ITEMS_NW", ...) -- this channel is a pure
 * pass-through to it, the same shape crafting_ui_impl and the uilist
 * takeover already established: send back the string, let C++ decide.
 * Nothing about move/sort/filter/examine logic is reimplemented here.
 *
 * Several actions open a nested screen of their own -- a filter text box
 * drawn inline into the pane's own curses window, a uilist for
 * sort/destination, the item-info window, a variable-amount prompt -- and
 * `process_action()` blocks synchronously until each of those resolves.
 * This channel is suspended around exactly those actions (see
 * `advanced_inventory::run_in_godot()`), so this panel is not what a player
 * sees on top of whichever one opens, the same move `MedicalSnapshot`'s
 * APPLY makes for the item picker.
 */
class AdvancedInvSnapshot
{
    public:
        bool active() const;
        uint64_t generation() const;

        // --- Godot thread -------------------------------------------------
        godot::Dictionary copy_state() const;
        void request_action( const std::string &action );
        /// A clicked row: which pane (0=left, 1=right) and its absolute
        /// index into that pane's published rows.
        void request_select( int side, int index );
        void note_attended();
        bool attended() const;

        // --- game thread ---------------------------------------------------
        struct row {
            std::string name;
            std::string amount;
            std::string weight;
            std::string volume;
            /// Non-empty when a category header belongs above this row
            /// (SORTBY_CATEGORY only).
            std::string category;
            bool favorite = false;
            bool autopickup = false;
        };
        struct area_button {
            /// The ITEMS_* (or ITEMS_DEFAULT-family) action this sends.
            std::string action;
            /// Two-letter key, e.g. "NW", "IN", "AL".
            std::string key;
            /// Full name, for a tooltip.
            std::string name;
            bool enabled = false;
        };
        struct pane_data {
            std::string area_name;
            std::string area_desc;
            std::string capacity;
            std::string filter;
            std::string sort_label;
            std::vector<row> rows;
            int selected = -1;
            int item_count = 0;
            int max_count = 0;
            bool active = false;
        };
        struct data {
            std::string title;
            pane_data left;
            pane_data right;
            bool category_mode = false;
            std::vector<area_button> areas;
        };

        void publish( const data &d );
        void set_suspended( bool suspended );
        void clear();

        /**
         * Block until the panel asks for something.
         *
         * @param side out: 0 or 1, the pane half of a row click.
         * @param index out: the row half of a row click.
         * @return the action string, "GODOT_SELECT" for a clicked row, or ""
         *         when no panel attended -- the caller must then run the
         *         legacy loop. Shutdown returns "QUIT".
         */
        std::string next_action( int &side, int &index );

    private:
        mutable std::mutex mutex_;
        bool active_ = false;
        data data_;
        uint64_t generation_ = 0;
        std::vector<std::string> pending_actions_;
        std::atomic<int> pending_side_{ -1 };
        std::atomic<int> pending_index_{ -1 };
        std::atomic<bool> suspended_{ false };
        std::atomic<bool> attended_{ false };
};

AdvancedInvSnapshot &get_advanced_inv_snapshot();

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_ADVANCED_INV_SNAPSHOT_H
