#pragma once
#ifndef CATA_SRC_GODOT_MEDICAL_SNAPSHOT_H
#define CATA_SRC_GODOT_MEDICAL_SNAPSHOT_H

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
 * The medical screen as a Godot Control.
 *
 * Same loop-split takeover as the others: `medical_ui::execute` keeps owning
 * every action -- limb selection, the tab, treating a wound, using an item --
 * and only the source of the action moves. The tab and the selected limb were
 * both lifted out of draw_controls() into process_action() first (the
 * "tab-in-widget" refactor from BACKLOG.md), so the members are authoritative
 * when an action is applied rather than when the ImGui frame next draws.
 *
 * Two of the actions open other screens, and they differ in kind:
 *
 * - "CONFIRM" (treat a wound) opens query_yn / uilist / popup, all of which are
 *   already Godot panels drawn on top of this one. The panel yields its keys to
 *   them while they are up.
 * - "APPLY" (use an item) opens the legacy inventory picker, which is still an
 *   overlay screen. The loop *suspends* this channel around that call -- active()
 *   goes false, the host hides the panel, and key forwarding to the legacy
 *   screen resumes -- the same move the dialogue takeover makes for the trade
 *   window. Without it the hidden half swallows every key and the picker can
 *   never be driven or dismissed.
 *
 * Limb detail (effects / wounds / scores) is published for the selected limb
 * only, exactly as the ImGui pane shows it -- get_limb_effects() words its
 * output against the *selected* part, so publishing it for every limb would
 * caption other limbs with the selected one's name.
 */
class MedicalSnapshot
{
    public:
        /// False while suspended, so the host hides the panel for the duration
        /// of a legacy sub-screen (see "APPLY" above).
        bool active() const;
        uint64_t generation() const;

        // --- Godot thread -------------------------------------------------
        godot::Dictionary copy_state() const;
        void request_action( const std::string &action );
        /// A clicked limb row, as an absolute index into the published list.
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
            int selected_limb = 0;
            /// Tagged limb names, HP bar and status flags included.
            std::vector<std::string> limb_names;
            /// The selected limb, as the detail pane shows it.
            std::string detail_title;
            std::string effects;
            std::string wounds;
            std::string scores;
            /// The summary tab.
            std::string speed_summary;
            std::string stats_summary;
            std::string weight_line;
        };

        void publish( const data &d );
        void set_suspended( bool suspended );
        void clear();

        /**
         * Block until the panel asks for something.
         *
         * @param select out: a limb row to select, or -1.
         * @param tab out: an absolute tab index to switch to, or -1.
         * @return the action string, "GODOT_SELECT" for a clicked limb,
         *         "GODOT_TAB" for a clicked tab, or "" when no panel attended --
         *         the caller must then run the legacy ImGui loop. Shutdown
         *         returns "QUIT".
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

MedicalSnapshot &get_medical_snapshot();

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_MEDICAL_SNAPSHOT_H
