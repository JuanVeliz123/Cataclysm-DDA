#pragma once
#ifndef CATA_SRC_GODOT_SCORES_SNAPSHOT_H
#define CATA_SRC_GODOT_SCORES_SNAPSHOT_H

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
 * The scores screen ("Your scores") as a Godot Control.
 *
 * Same shape as the surroundings takeover: `scores_ui::draw_scores_ui` is a
 * loop that reads an action and hands it to the window, so only the source of
 * the action moves. The content -- achievements, conducts, scores, kills -- is
 * built once by `init_data()` and never changes while the screen is up; what
 * changes is which tab is selected and whether the two kill groups are
 * collapsed, all of which now lives in members the game thread owns (the tab
 * used to be chosen by `BeginTabItem` returning true, which nothing off the
 * ImGui thread could read; see the lift in scores_ui.cpp).
 *
 * Every string is published with its CDDA colour tags intact -- the panel
 * renders them through color_tags.gd, because in the kill list the colour is
 * the monster's identity, not decoration.
 */
class ScoresSnapshot
{
    public:
        bool active() const;
        uint64_t generation() const;

        // --- Godot thread -------------------------------------------------
        godot::Dictionary copy_state() const;
        void request_action( const std::string &action );
        /// A clicked tab arrives as an absolute index; keys go through
        /// request_action( "NEXT_TAB" / "PREV_TAB" ) like the ImGui screen.
        void request_tab( int index );
        void note_attended();
        bool attended() const;

        // --- game thread ---------------------------------------------------
        struct kill_row {
            int count = 0;
            std::string symbol;
            /// Colour name from string_from_color(); the panel maps it.
            std::string color;
            std::string name;
        };
        /// One text tab: the rows, what to say instead when there are none, and
        /// the trailing note (empty when the ImGui screen would not draw one).
        struct text_tab {
            std::vector<std::string> rows;
            std::string empty_text;
            std::string note;
        };
        struct data {
            std::string title;
            std::vector<std::string> tab_titles;
            int selected_tab = 0;
            text_tab achievements;
            text_tab conducts;
            text_tab scores;
            std::string monster_header;
            std::string monster_empty;
            std::vector<kill_row> monster_kills;
            std::string npc_header;
            std::string npc_empty;
            std::vector<std::string> npc_kills;
            bool monster_collapsed = false;
            bool npc_collapsed = false;
            int total_kills = 0;
        };

        void publish( const data &d );
        void clear();

        /**
         * Block until the panel asks for something.
         *
         * @param tab out: an absolute tab index to switch to, or -1.
         * @return the action string, "GODOT_TAB" for a clicked tab, or "" when
         *         no panel attended within the grace period -- the caller must
         *         then run the legacy ImGui screen. Shutdown returns "QUIT".
         */
        std::string next_action( int &tab );

    private:
        mutable std::mutex mutex_;
        bool active_ = false;
        data data_;
        uint64_t generation_ = 0;
        std::vector<std::string> pending_actions_;
        std::atomic<int> pending_tab_{ -1 };
        std::atomic<bool> attended_{ false };
};

ScoresSnapshot &get_scores_snapshot();

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_SCORES_SNAPSHOT_H
