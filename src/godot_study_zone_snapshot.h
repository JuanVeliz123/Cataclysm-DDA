#pragma once
#ifndef CATA_SRC_GODOT_STUDY_ZONE_SNAPSHOT_H
#define CATA_SRC_GODOT_STUDY_ZONE_SNAPSHOT_H

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
 * The study zone skill-preference grid (skills x followers) as a Godot Control.
 *
 * Same loop-split takeover as MedicalSnapshot, but simpler: there is no tab to
 * lift out of draw_controls(), and a checkbox toggle is stateless -- it mutates
 * `npc_skill_preferences` directly rather than moving a cursor first, exactly as
 * `draw_skill_row()` does under ImGui. What *is* lifted out is the skill-name
 * filter, because it decides which rows exist at all and the panel types it a
 * character at a time -- the same `set_filter`/`take_filter` dirty-flag contract
 * as `UilistSnapshot`.
 *
 * A toggle addresses a skill by its absolute index into the unfiltered skill
 * list, not by row position, so a toggle that arrives the instant after a
 * filter change can never land on the wrong skill.
 */
class StudyZoneSnapshot
{
    public:
        bool active() const;
        uint64_t generation() const;

        // --- Godot thread -------------------------------------------------
        godot::Dictionary copy_state() const;
        void request_action( const std::string &action );
        /// A clicked checkbox: absolute skill index, absolute npc-column index.
        void request_toggle( int skill_index, int npc_index );
        void set_filter( const std::string &text );
        void note_attended();
        bool attended() const;

        // --- game thread ---------------------------------------------------
        struct row {
            /// Absolute index into the unfiltered skill list -- stable across
            /// filtering, and what a toggle addresses.
            int skill_index = 0;
            std::string skill_name;
            /// Aligned with `data::npc_names`.
            std::vector<bool> checked;
        };
        struct data {
            std::string title;
            std::vector<std::string> npc_names;
            /// Already filtered, in display order.
            std::vector<row> rows;
            std::string filter;
        };

        void publish( const data &d );
        void clear();

        /**
         * Block until the panel asks for something.
         *
         * @param skill_index out: the skill half of a toggle, or -1.
         * @param npc_index out: the npc-column half of a toggle, or -1.
         * @param filter_text out: new filter text, when the return is
         *        "GODOT_FILTER".
         * @return the action string, "GODOT_TOGGLE" for a clicked checkbox,
         *         "GODOT_FILTER" for new filter text, or "" when no panel
         *         attended -- the caller must then run the legacy ImGui loop.
         *         Shutdown returns "QUIT".
         */
        std::string next_action( int &skill_index, int &npc_index, std::string &filter_text );

    private:
        mutable std::mutex mutex_;
        bool active_ = false;
        data data_;
        uint64_t generation_ = 0;
        std::vector<std::string> pending_actions_;
        std::atomic<int> pending_toggle_skill_{ -1 };
        std::atomic<int> pending_toggle_npc_{ -1 };
        std::string filter_;
        bool filter_dirty_ = false;
        std::atomic<bool> attended_{ false };
};

StudyZoneSnapshot &get_study_zone_snapshot();

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_STUDY_ZONE_SNAPSHOT_H
