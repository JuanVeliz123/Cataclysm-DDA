#pragma once
#ifndef CATA_SRC_GODOT_MISSION_SNAPSHOT_H
#define CATA_SRC_GODOT_MISSION_SNAPSHOT_H

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
 * The missions screen ("Your missions") as a Godot Control.
 *
 * Combines the two shapes already established: a tab bar like ScoresSnapshot
 * (four tabs -- active, completed, failed, points of interest -- previously
 * chosen only by `BeginTabItem` returning true, which nothing off the ImGui
 * thread could read) and a row cursor like MedicalSnapshot (which mission in
 * a tab is selected, and the detail pane built against it).
 *
 * Unlike the other tab screens, which tab is selected here changes what
 * *rows exist at all* -- active/completed/failed missions and points of
 * interest are different collections, not different views of the same one --
 * so a tab switch always republishes a fresh row list and resets the cursor
 * to the top, exactly as the ImGui screen's NEXT_TAB/PREV_TAB handling does.
 */
class MissionSnapshot
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
            /// Mission names, or point-of-interest text, for the active tab.
            std::vector<std::string> rows;
            /// Shown instead of the table when `rows` is empty.
            std::string empty_text;
            /// "Current objective: ..." / "Current point of interest: ...",
            /// empty when neither is set.
            std::string current_objective;
            /// The selected row's detail pane, colour tags intact.
            std::string detail;
            /// Whether DELETE_POINT_OF_INTEREST applies on this tab/row.
            bool can_delete = false;
        };

        void publish( const data &d );
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
        std::atomic<bool> attended_{ false };
};

MissionSnapshot &get_mission_snapshot();

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_MISSION_SNAPSHOT_H
