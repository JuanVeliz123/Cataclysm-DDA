#pragma once
#ifndef CATA_SRC_GODOT_KEYBIND_SNAPSHOT_H
#define CATA_SRC_GODOT_KEYBIND_SNAPSHOT_H

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
 * The keybindings screen as a Godot Control.
 *
 * One generation, unlike the options channel: a binding changes rarely -- a few
 * times per visit rather than on every arrow key -- so republishing the list is
 * cheap, and the panel keeps its place by remembering the selected action id
 * rather than a row number.
 *
 * Adding a binding is the part that does not fit the request/response shape: the
 * game has to learn which key was pressed, and it has to arrive as the very same
 * `input_event` the game will later compare against, or the new binding will not
 * match when the player uses it. So the panel does not describe the key. The
 * prompt goes up on the notice channel, the panel forwards the raw Godot event to
 * the input bridge -- the one place that knows how to translate one -- and the
 * game thread reads the translated event back out. See
 * godot_backend::run_anykey_popup_in_godot.
 */
class KeybindSnapshot
{
    public:
        bool active() const;
        uint64_t generation() const;

        // --- Godot thread -------------------------------------------------
        godot::Dictionary copy_state() const;
        /// What to do with @p action_id. See `operation`.
        void request( const std::string &action_id, int op );
        void dismiss();
        void note_attended();
        bool attended() const;

        // --- game thread ---------------------------------------------------
        struct row {
            std::string action_id;
            std::string name;
            /// The bound keys, already formatted for display.
            std::string keys;
            /// 0 global, 1 local to this context, 2 unbound.
            int scope = 0;
            /// True when the player has changed this from the shipped default.
            bool customized = false;
        };
        enum class operation : int {
            remove = 0,
            reset = 1,
            add_local = 2,
            add_global = 3,
            execute = 4,
        };

        void publish( const std::string &context, const std::vector<row> &rows,
                      bool permit_execute );
        void clear();

        struct pending {
            std::string action_id;
            operation op = operation::remove;
        };
        std::vector<pending> take_requests();
        bool dismissed() const;

    private:
        void bump();

        mutable std::mutex mutex_;
        bool active_ = false;
        std::string context_;
        std::vector<row> rows_;
        bool permit_execute_ = false;
        std::vector<pending> requests_;
        uint64_t generation_ = 0;
        std::atomic<bool> dismissed_{ false };
        std::atomic<bool> attended_{ false };
};

KeybindSnapshot &get_keybind_snapshot();

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_KEYBIND_SNAPSHOT_H
