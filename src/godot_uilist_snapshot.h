#pragma once
#ifndef CATA_SRC_GODOT_UILIST_SNAPSHOT_H
#define CATA_SRC_GODOT_UILIST_SNAPSHOT_H

#if defined(GODOT)

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <godot_cpp/variant/dictionary.hpp>

class uilist;

namespace godot_backend
{

/**
 * `uilist` rendered as a Godot Control instead of an ImGui window.
 *
 * `uilist` is the single mechanism behind most of the game's menus -- 254 call
 * sites, of which only a handful use a callback or categories. Migrating it once
 * moves nearly all of them off the curses/ImGui overlay together, which is a very
 * different proposition from rewriting 75 files by hand.
 *
 * The game thread blocks inside uilist::query() as it always did. Instead of
 * driving an ImGui frame it publishes the entry list here, waits for Godot to
 * answer, and returns the chosen retval. Nothing about the callers changes.
 *
 * Menus this cannot faithfully reproduce -- ones with a uilist_callback that
 * draws its own panes or eats keys, and ones with category tabs -- keep the old
 * path. @ref can_take_over is that test, and it is deliberately conservative:
 * a menu that renders slightly wrong is worse than one that still renders the
 * old way.
 */
class UilistSnapshot
{
    public:
        struct entry {
            std::string text;
            /// Second column, e.g. a quantity or a keybinding hint.
            std::string ctxt;
            /// Longer text for the description pane, when desc_enabled.
            std::string desc;
            /// Displayed hotkey, empty when the entry has none.
            std::string hotkey;
            bool enabled = true;
            /// Index into uilist::entries, so a click maps back unambiguously
            /// even when a filter is hiding rows.
            int index = 0;
        };

        /// Whether a Godot panel should be showing a menu right now.
        bool active() const;
        /// Called whenever Godot reads the state, i.e. proof a panel is showing
        /// this menu. Without it the game thread has no way to tell "the player
        /// has not chosen yet" from "nothing is rendering this", and the second
        /// case would block the game forever.
        void note_attended();
        bool attended() const;
        /// Bumped on every publish; the panel polls it to know when to rebuild.
        uint64_t generation() const;
        godot::Dictionary copy_state() const;

        // --- Godot thread -> game thread ---------------------------------
        /// Move the highlight. Index is into uilist::entries.
        void set_selected( int index );
        /// Switch to category tab @p index.
        void select_category( int index );
        int requested_category() const;
        /// Accept @p index (or the current highlight when negative).
        void confirm( int index );
        /// Dismiss without choosing.
        void cancel();
        void set_filter( const std::string &text );

        // --- game thread -------------------------------------------------
        void publish( const uilist &menu, const std::vector<entry> &rows, int selected );
        /// Category tab labels and which is showing. Published alongside rows.
        void set_categories( const std::vector<std::string> &labels, int current );
        void clear();
        /// -1 while the player has not answered yet.
        enum class answer : int8_t { pending, chosen, cancelled };
        answer take_answer( int &index );
        /// Latest filter text from the panel, and whether it changed.
        bool take_filter( std::string &out );
        int requested_selection() const;

    private:
        mutable std::mutex mutex_;
        bool active_ = false;
        std::string title_;
        std::string text_;
        std::string footer_;
        bool desc_enabled_ = false;
        bool filtering_ = false;
        std::string filter_;
        bool filter_dirty_ = false;
        std::vector<entry> entries_;
        std::vector<std::string> categories_;
        int current_category_ = 0;
        int selected_ = 0;
        std::atomic<int> requested_category_{ -1 };
        std::atomic<int> requested_selection_{ -1 };
        std::atomic<int> answer_index_{ -1 };
        std::atomic<int8_t> answer_{ static_cast<int8_t>( answer::pending ) };
        uint64_t generation_ = 0;
        std::atomic<bool> attended_{ false };
};

UilistSnapshot &get_uilist_snapshot();

/**
 * Run @p menu as a Godot panel.
 *
 * @return true when it handled the menu and set `menu.ret`; false when the menu
 *         needs the legacy path.
 */
bool run_uilist_in_godot( uilist &menu );

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_UILIST_SNAPSHOT_H
