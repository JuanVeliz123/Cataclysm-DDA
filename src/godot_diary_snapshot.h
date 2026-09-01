#pragma once
#ifndef CATA_SRC_GODOT_DIARY_SNAPSHOT_H
#define CATA_SRC_GODOT_DIARY_SNAPSHOT_H

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
 * The diary screen (`diary::show_diary_ui`) as a Godot Control.
 *
 * Same loop-split takeover as the other MENU-13 screens: `show_diary_ui`
 * blocks in `run_in_godot` while a panel is attending, and falls back to the
 * legacy curses loop when it is not.
 *
 * The legacy screen has three keyboard-focusable panes (pages / changes /
 * text) cycled with LEFT/RIGHT. The panel does not reproduce that -- a click
 * addresses a page or a change row directly, which is the same
 * simplified-re-presentation call the advanced-inventory and martial-arts
 * panels made. `selected_change` still exists because on the summary page it
 * decides which entry's text shows in the detail pane (see
 * `diary::get_desc_or_page_text`); on an ordinary page the detail pane always
 * shows the page's free text and the value is unused.
 *
 * Two actions leave this screen for a legacy one that has not been migrated
 * (or, for scores, has its own channel this one must get out from under):
 * "EDIT_TEXT" opens `string_editor_window`, a raw `catacurses::window`, and
 * "VIEW_SCORES" opens the already-migrated scores screen. Both suspend this
 * channel first, the same move `MedicalSnapshot`'s APPLY makes for the item
 * picker.
 */
class DiarySnapshot
{
    public:
        bool active() const;
        uint64_t generation() const;

        // --- Godot thread -------------------------------------------------
        godot::Dictionary copy_state() const;
        void request_action( const std::string &action );
        void request_select_page( int page_index );
        void request_select_change( int change_index );
        void note_attended();
        bool attended() const;

        /// False while suspended, so the host hides the panel for the
        /// duration of a legacy sub-screen (the text editor, the scores
        /// screen).
        bool suspended() const;
        void set_suspended( bool suspended );

        // --- game thread ---------------------------------------------------
        struct data {
            std::string title;
            std::string head_text;
            std::vector<std::string> pages;
            int current_page = -1;
            bool is_summary = false;
            std::vector<std::string> changes;
            /// Parallel to `changes`; empty when a row has no description.
            std::vector<std::string> change_desc;
            int selected_change = 0;
            /// Free-text page body; only meaningful when `!is_summary`.
            std::string page_text;
            std::string hint;
        };

        void publish( const data &d );
        void clear();

        /**
         * Block until the panel asks for something.
         *
         * @param page_index out: the page half of "GODOT_SELECT_PAGE", or -1.
         * @param change_index out: the change half of "GODOT_SELECT_CHANGE",
         *        or -1.
         * @return the action string, or "" when no panel attended -- the
         *         caller must then run the legacy curses loop. Shutdown
         *         returns "QUIT".
         */
        std::string next_action( int &page_index, int &change_index );

    private:
        mutable std::mutex mutex_;
        bool active_ = false;
        data data_;
        uint64_t generation_ = 0;
        std::vector<std::string> pending_actions_;
        std::atomic<int> pending_select_page_{ -1 };
        std::atomic<int> pending_select_change_{ -1 };
        std::atomic<bool> attended_{ false };
        std::atomic<bool> suspended_{ false };
};

DiarySnapshot &get_diary_snapshot();

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_DIARY_SNAPSHOT_H
