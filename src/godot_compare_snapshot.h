#pragma once
#ifndef CATA_SRC_GODOT_COMPARE_SNAPSHOT_H
#define CATA_SRC_GODOT_COMPARE_SNAPSHOT_H

#if defined(GODOT)

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include <godot_cpp/variant/dictionary.hpp>

namespace godot_backend
{

/**
 * `compare_item_menu` -- two items' info panes shown side by side, with an
 * optional confirm/quit bar -- as a Godot Control.
 *
 * No selection state: each pane is independently scrolled read-only text,
 * the same shape `TextWinSnapshot` already serves for a single pane (item
 * info, extended description, help), just two of them at once. Kept as its
 * own class rather than extending `TextWinSnapshot` to avoid touching that
 * well-exercised single-pane path for a two-pane layout only this screen
 * needs.
 */
class CompareSnapshot
{
    public:
        bool active() const;
        uint64_t generation() const;

        // --- Godot thread ---------------------------------------------------
        godot::Dictionary copy_state() const;
        /// @p action is "CONFIRM" or "QUIT".
        void request_action( const std::string &action );
        void note_attended();
        bool attended() const;

        // --- game thread ------------------------------------------------------
        struct data {
            std::string title;
            std::string first_name;
            std::string first_body;
            std::string second_name;
            std::string second_body;
            /// Empty when there is nothing to confirm -- just a read-only
            /// comparison with no CONFIRM/QUIT bar (mirrors the legacy
            /// window's own `confirm_message.empty()` check).
            std::string confirm_message;
        };

        void publish( const data &d );
        void clear();

        /**
         * Block until CONFIRM, QUIT, or shutdown.
         * @return "CONFIRM", "QUIT", or "" when no panel attended within the
         *         deadline, meaning the caller must run the legacy ImGui
         *         loop instead.
         */
        std::string next_action();

    private:
        mutable std::mutex mutex_;
        bool active_ = false;
        data data_;
        uint64_t generation_ = 0;
        // 0 = none, 1 = confirm, 2 = quit.
        std::atomic<int> pending_action_{ 0 };
        std::atomic<bool> attended_{ false };
};

CompareSnapshot &get_compare_snapshot();

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_COMPARE_SNAPSHOT_H
