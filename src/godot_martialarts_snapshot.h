#pragma once
#ifndef CATA_SRC_GODOT_MARTIALARTS_SNAPSHOT_H
#define CATA_SRC_GODOT_MARTIALARTS_SNAPSHOT_H

#if defined(GODOT)

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <godot_cpp/variant/dictionary.hpp>

#include "type_id.h"

class Character;

namespace godot_backend
{

/**
 * The martial-arts style picker as a Godot Control.
 *
 * The legacy screen is a uilist over the avatar's styles whose row 0 is the
 * "keep hands free" toggle, plus an ImGui window (`ma_details_ui_impl` in
 * martialarts.cpp) the player opens per style for techniques, buffs and
 * compatible weapons. The Godot panel folds the two together: a list on the
 * left, and the full details of whatever the cursor is on as a permanent pane
 * on the right.
 *
 * Shape: model + loop, like options. `character_martial_arts::pick_style`
 * builds the style list, asks `run_martialarts_in_godot` to stand in for the
 * uilist query, and applies the returned selection through the same epilogue
 * the legacy path uses -- so activating a style, toggling keep-hands-free and
 * cancelling behave identically whichever front end answered.
 *
 * The detail pane is published as colour-tagged lines the way the crafting
 * detail is: the text comes from the same code the ImGui window renders
 * (`ma_style_details_lines`, defined in martialarts.cpp), so the two screens
 * cannot drift apart, and the panel renders the `<color_...>` tags via
 * color_tags.gd rather than re-deriving game rules in GDScript.
 */
class MartialArtsSnapshot
{
    public:
        bool active() const;
        uint64_t generation() const;

        // --- Godot thread -------------------------------------------------
        /// The whole screen. Reading it marks the channel attended.
        godot::Dictionary copy_state() const;
        /// "UP", "DOWN", "PAGE_UP", "PAGE_DOWN", "HOME", "END", "SELECT",
        /// "QUIT". Queued in order; the game thread applies each and
        /// republishes, so the panel sees the selection move.
        void request_action( const std::string &action );
        /// Move the cursor straight to @p index (a click on a row).
        void request_move_to( int index );
        /// Activate row @p index: cursor moved and SELECT applied in one step,
        /// so the game cannot act on a half-applied selection.
        void request_select( int index );
        void note_attended();
        bool attended() const;

        // --- game thread ---------------------------------------------------
        struct row {
            std::string name;
            /// The style the character currently uses.
            bool active = false;
            /// The keep-hands-free toggle rather than a style.
            bool toggle = false;
        };
        struct detail_line {
            /// Colour-tagged (`<color_...>`), the way crafting detail lines are.
            std::string text;
            /// A section header ("Techniques (4)") rather than body text.
            bool header = false;
        };

        void publish( const std::string &title, const std::string &subtitle,
                      const std::vector<row> &rows, int selection,
                      const std::vector<detail_line> &detail, bool keep_hands_free );
        void clear();

        /// Everything the panel has asked for since the last call, in order.
        std::vector<std::string> take_actions();
        /// The row a click moved the cursor to, or -1.
        int take_move_to();
        /// The row a click activated, or -1.
        int take_select();

    private:
        mutable std::mutex mutex_;
        bool active_ = false;
        std::string title_;
        std::string subtitle_;
        std::vector<row> rows_;
        int selection_ = 0;
        std::vector<detail_line> detail_;
        bool keep_hands_free_ = false;
        uint64_t generation_ = 0;
        std::vector<std::string> pending_actions_;
        std::atomic<int> pending_move_to_{ -1 };
        std::atomic<int> pending_select_{ -1 };
        std::atomic<bool> attended_{ false };
};

MartialArtsSnapshot &get_martialarts_snapshot();

/**
 * The details pane's content for one style, as colour-tagged lines.
 *
 * Defined in martialarts.cpp so it shares the gathering code with the ImGui
 * details window rather than re-deriving what a buff or technique says.
 */
std::vector<MartialArtsSnapshot::detail_line> ma_style_details_lines( const matype_id &style );

/**
 * Stand in for the style-picker uilist and block until the panel answers.
 *
 * @param you             whose stats the header shows (the avatar, or an NPC
 *                        whose style is being picked through dialogue).
 * @param styles          the selectable styles, already sorted; row i + 1 on
 *                        the published list is styles[i], row 0 is the
 *                        keep-hands-free toggle.
 * @param current_style   marked active on the published list.
 * @param keep_hands_free current value, shown on the toggle row's label.
 * @param initial_selection where the cursor starts (the current style).
 * @param ret             out: the uilist-compatible outcome -- 0 for the
 *                        keep-hands-free toggle, i + 1 for styles[i], and -1
 *                        for cancelled. Only meaningful when true is returned.
 * @return false when no panel attended, so the caller must run the legacy
 *         uilist instead.
 */
bool run_martialarts_in_godot( const Character &you, const std::vector<matype_id> &styles,
                               const matype_id &current_style, bool keep_hands_free,
                               int initial_selection, int &ret );

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_MARTIALARTS_SNAPSHOT_H
