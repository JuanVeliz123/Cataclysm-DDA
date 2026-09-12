#pragma once
#ifndef CATA_SRC_GODOT_FOLLOWER_RULES_SNAPSHOT_H
#define CATA_SRC_GODOT_FOLLOWER_RULES_SNAPSHOT_H

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
 * Follower rules (MENU-13's last screen) as a Godot Control.
 *
 * Unlike every other MENU-13 screen, the legacy ImGui window has no selection
 * state to lift out -- CONFIRM toggles whatever ImGui's own keyboard-nav focus
 * happens to sit on (`ImGui::GetActiveID()`). But every rule is *also* directly
 * hotkey-addressable (`draw_controls()`'s `pressed_key == assigned_hotkey`
 * path), so a mouse-driven panel never needs a cursor at all: a boolean rule is
 * toggled by its stable `ally_rule` flag value, and a radio group is set by its
 * enum's own int value -- exactly what a keypress already does, just addressed
 * directly instead of by nav focus.
 *
 * IMPORT/EXPORT -- the rule-transfer popup that copies settings to or from
 * another follower -- is not carried over. It is its own sub-screen (a second
 * table, not a simple action), and is rare enough (needs 2+ followers) that it
 * is left for a follow-up rather than folded into this pass. See BACKLOG.md.
 */
class FollowerRulesSnapshot
{
    public:
        bool active() const;
        uint64_t generation() const;

        // --- Godot thread -------------------------------------------------
        godot::Dictionary copy_state() const;
        void request_action( const std::string &action );
        /// Toggle one boolean rule, addressed by its `ally_rule` flag value.
        void request_toggle( int rule_flag );
        /// Reset one boolean rule to its default, addressed the same way.
        void request_default_rule( int rule_flag );
        /// Set one radio-group rule. @p group is "engagement" / "aim" /
        /// "cbm_recharge" / "cbm_reserve"; @p value is that enum's own int
        /// value, the same one `copy_state()` published for each option.
        void request_set( const std::string &group, int value );
        void note_attended();
        bool attended() const;

        // --- game thread ---------------------------------------------------
        struct bool_rule {
            /// The `ally_rule` flag value -- stable, and what a toggle
            /// addresses, unlike the iteration index of the `unordered_map`
            /// this is built from.
            int flag = 0;
            std::string label;
            std::string hotkey;
            bool enabled = false;
        };
        struct radio_option {
            int value = 0;
            std::string label;
        };
        struct radio_group {
            std::string id;
            std::string title;
            std::string hotkey;
            int current = 0;
            std::vector<radio_option> options;
        };
        struct data {
            std::string title;
            std::vector<bool_rule> rules;
            std::vector<radio_group> groups;
        };

        void publish( const data &d );
        void clear();

        /**
         * Block until the panel asks for something.
         *
         * @param rule_flag out: the `ally_rule` flag half of a toggle or a
         *        per-rule default, or 0.
         * @param group out: the radio-group id, when the return is
         *        "GODOT_SET".
         * @param value out: the radio-group value, when the return is
         *        "GODOT_SET".
         * @return "GODOT_TOGGLE", "GODOT_DEFAULT_RULE", "GODOT_SET",
         *         "DEFAULT_ALL", "QUIT", or "" when no panel attended -- the
         *         caller must then run the legacy ImGui loop.
         */
        std::string next_action( int &rule_flag, std::string &group, int &value );

    private:
        mutable std::mutex mutex_;
        bool active_ = false;
        data data_;
        uint64_t generation_ = 0;
        std::vector<std::string> pending_actions_;
        std::atomic<int> pending_toggle_flag_{ 0 };
        std::atomic<int> pending_default_flag_{ 0 };
        std::string pending_set_group_;
        int pending_set_value_ = 0;
        bool set_dirty_ = false;
        std::atomic<bool> attended_{ false };
};

FollowerRulesSnapshot &get_follower_rules_snapshot();

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_FOLLOWER_RULES_SNAPSHOT_H
