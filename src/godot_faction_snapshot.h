#pragma once
#ifndef CATA_SRC_GODOT_FACTION_SNAPSHOT_H
#define CATA_SRC_GODOT_FACTION_SNAPSHOT_H

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
 * The faction screen ("Faction") as a Godot Control.
 *
 * Four tabs (your camps, your followers, other factions, known creatures),
 * each backed by a different collection and a different pointer type
 * (`basecamp *`, `npc *`, `const faction *`, `const mtype_id *`) -- the
 * ImGui screen keeps one `picked_*` member per tab rather than one shared
 * cursor, and this channel does the same: `selected_row` is always an index
 * into whichever list the current tab published, and `select_row` on the
 * game thread re-derives the right `picked_*` from it.
 *
 * Unlike every other tab screen migrated so far, this one needed no tab-lift
 * at all -- `faction_ui` already used `cataimgui::BeginTabItem(label,
 * selected_tab == X)` instead of reading `BeginTabItem`'s return value, so
 * `selected_tab` was already the single source of truth before this existed.
 * What still needed lifting was the row cursor math, which lived inline in
 * each tab's own list-drawing function.
 *
 * `CONFIRM` always ends the screen here (mirrors `faction_ui::execute()`,
 * which returns unconditionally once a CONFIRM is handled, whichever tab it
 * came from) and may open a legacy or Godot sub-screen first (dialogue, a
 * uilist, a rename prompt) -- the channel is suspended around that call so
 * this panel is not what a player sees on top of it, the same move
 * `MedicalSnapshot`'s APPLY makes for the item picker.
 */
class FactionSnapshot
{
    public:
        bool active() const;
        uint64_t generation() const;

        // --- Godot thread -------------------------------------------------
        godot::Dictionary copy_state() const;
        void request_action( const std::string &action );
        /// A clicked row, absolute index into the currently published list.
        void request_select( int index );
        /// A clicked tab, absolute; keys go through "NEXT_TAB" / "PREV_TAB".
        void request_tab( int index );
        void note_attended();
        bool attended() const;

        // --- game thread ---------------------------------------------------
        struct data {
            std::string title;
            std::vector<std::string> tab_titles;
            int selected_tab = 0;
            int selected_row = 0;
            /// Row labels for the active tab only.
            std::vector<std::string> rows;
            /// The selected row's detail pane -- or, when `rows` is empty,
            /// the "you have none of these" message the ImGui pane draws in
            /// its place. Colour tags intact.
            std::string detail;
        };

        void publish( const data &d );
        void set_suspended( bool suspended );
        void clear();

        /**
         * Block until the panel asks for something.
         *
         * @param select out: a row to select, or -1.
         * @param tab out: an absolute tab index to switch to, or -1.
         * @return the action string, "GODOT_SELECT" for a clicked row,
         *         "GODOT_TAB" for a clicked tab, or "" when no panel
         *         attended -- the caller must then run the legacy ImGui
         *         loop. Shutdown returns "QUIT".
         */
        std::string next_action( int &select, int &tab );

    private:
        mutable std::mutex mutex_;
        bool active_ = false;
        data data_;
        uint64_t generation_ = 0;
        std::vector<std::string> pending_actions_;
        std::atomic<int> pending_select_{ -1 };
        std::atomic<int> pending_tab_{ -1 };
        std::atomic<bool> suspended_{ false };
        std::atomic<bool> attended_{ false };
};

FactionSnapshot &get_faction_snapshot();

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_FACTION_SNAPSHOT_H
