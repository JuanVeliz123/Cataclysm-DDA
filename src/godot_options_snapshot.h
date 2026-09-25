#pragma once
#ifndef CATA_SRC_GODOT_OPTIONS_SNAPSHOT_H
#define CATA_SRC_GODOT_OPTIONS_SNAPSHOT_H

#if defined(GODOT)

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <godot_cpp/variant/dictionary.hpp>

#include "options.h"

namespace godot_backend
{

/**
 * The options screen as a Godot Control.
 *
 * This channel is shaped differently from the read-only ones, because options are
 * *edited*: Godot never owns a value, it asks the game thread to change one and
 * reads back what actually happened. That matters because a `cOpt` can refuse or
 * clamp what it is given -- a float has a step, an int has a range, and an option
 * with an unmet prerequisite ignores the write entirely. Echoing the typed value
 * back into the widget would show a value the game does not hold.
 *
 * Two generations rather than one:
 *
 * - **layout** changes only when the screen is opened. It is the pages, the group
 *   headers and the rows, which do not move while the screen is up.
 * - **values** changes on every applied edit. It is what each option currently
 *   reads as, and whether its prerequisite is met.
 *
 * Splitting them keeps a single toggle from rebuilding a few hundred Control
 * nodes, and keeps the panel's scroll position and keyboard row where the player
 * left them.
 *
 * Tab selection and group collapse are not here at all: Godot has the whole model
 * and can do both without a round trip.
 */
class OptionsSnapshot
{
    public:
        bool active() const;
        uint64_t layout_generation() const;
        uint64_t values_generation() const;

        // --- Godot thread -------------------------------------------------
        /// Pages, groups and rows. Read once per layout generation.
        godot::Dictionary copy_layout() const;
        /// option name -> { value, value_name, enabled }. Read per values generation.
        godot::Dictionary copy_values() const;

        /// Set @p option to a literal value (string_input, and string_select from
        /// a dropdown). Applied on the game thread, which may clamp or refuse it.
        void request_set( const std::string &option, const std::string &value );
        /// Step @p option by @p delta places: bool toggles, numerics move one step,
        /// select and int_map move one entry. This is what LEFT/RIGHT does.
        void request_step( const std::string &option, int delta );
        void dismiss();
        /// Proof a panel is rendering this; see the uilist snapshot for why the
        /// game thread cannot safely wait without it.
        void note_attended();
        bool attended() const;

        // --- game thread ---------------------------------------------------
        struct row {
            /// Mirrors options_manager::ItemType.
            enum class kind : int8_t { blank, group, option };
            kind type = kind::blank;
            /// Option name, or group id for a header.
            std::string id;
            std::string text;
            std::string tooltip;
            /// Owning group, empty when the row is not in one.
            std::string group;
            /// "bool", "string_select", "string_input", "int", "int_map", "float".
            std::string value_type;
            std::string default_text;
            int max_length = 0;
            /// Choices for string_select, as (value, label).
            std::vector<std::pair<std::string, std::string>> items;
        };
        struct page {
            std::string id;
            std::string name;
            std::vector<row> rows;
        };

        /// What an option currently reads as. Kept apart from `row` because this
        /// is the half that changes while the screen is up.
        struct value {
            std::string current;
            std::string display;
            /// False when a prerequisite is unmet; the row is shown but inert,
            /// matching the curses screen, which refuses the edit with a popup.
            bool enabled = true;
        };

        void publish( const std::vector<page> &pages, int current_page, bool allow_tabs );
        void publish_values( std::vector<std::pair<std::string, value>> values );
        void clear();

        struct edit {
            std::string option;
            /// 0 means "use `value`"; otherwise the number of places to step.
            int delta = 0;
            std::string value;
        };
        /// Hand over everything Godot has asked for since the last call.
        std::vector<edit> take_edits();
        bool dismissed() const;

    private:
        mutable std::mutex mutex_;
        bool active_ = false;
        std::vector<page> pages_;
        int current_page_ = 0;
        bool allow_tabs_ = true;
        std::vector<std::pair<std::string, value>> values_;
        std::vector<edit> edits_;
        uint64_t layout_generation_ = 0;
        uint64_t values_generation_ = 0;
        std::atomic<bool> dismissed_{ false };
        std::atomic<bool> attended_{ false };
};

OptionsSnapshot &get_options_snapshot();

/**
 * Show the options screen as a Godot panel and block until it is dismissed.
 *
 * Edits are applied to the same option objects the curses screen edits, so
 * options_manager::show() detects and applies the changes afterwards either way.
 *
 * @param global the global option container.
 * @param world  the active world's options, or @p global when there is no world.
 *               Which of the two an option belongs to follows the same rule the
 *               curses screen uses: the world container for the world_default
 *               page while in a game, the global one otherwise.
 * @return false when no panel attended, so the caller must run its own screen.
 */
bool run_options_in_godot( options_manager &opts,
                           options_manager::options_container &global,
                           options_manager::options_container &world,
                           bool ingame, bool with_tabs );

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_OPTIONS_SNAPSHOT_H
