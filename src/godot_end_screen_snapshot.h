#pragma once
#ifndef CATA_SRC_GODOT_END_SCREEN_SNAPSHOT_H
#define CATA_SRC_GODOT_END_SCREEN_SNAPSHOT_H

#if defined(GODOT)

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include <godot_cpp/variant/dictionary.hpp>

namespace godot_backend
{

/**
 * The end-of-game screen (an ascii-art tombstone plus an optional "last
 * words" field) as a Godot Control.
 *
 * No selection state, and simpler than FollowerRulesSnapshot's own "nothing
 * to pick" shape: there isn't even a set of addressable rules, just one
 * static body (the art, with any per-screen overlay text already spliced
 * into it -- see `compose_end_screen_body()` in end_screen.cpp) and one
 * optional text field. CONFIRM is the only real outcome.
 */
class EndScreenSnapshot
{
    public:
        bool active() const;
        uint64_t generation() const;

        // --- Godot thread ---------------------------------------------------
        godot::Dictionary copy_state() const;
        void request_confirm( const std::string &text );
        void note_attended();
        bool attended() const;

        // --- game thread ------------------------------------------------------
        struct data {
            std::string body;
            bool has_text_input = false;
            std::string text_label;
        };

        void publish( const data &d );
        void clear();

        /**
         * Block until CONFIRM (with whatever text was typed, if any) or
         * shutdown.
         * @param text_out the text field's value when the return is
         *        "CONFIRM"; empty otherwise.
         * @return "CONFIRM", "QUIT" (shutdown requested -- treat like an
         *         empty confirm rather than parking the game thread), or ""
         *         when no panel attended within the deadline, meaning the
         *         caller must run the legacy ImGui loop instead.
         */
        std::string next_action( std::string &text_out );

    private:
        mutable std::mutex mutex_;
        bool active_ = false;
        data data_;
        uint64_t generation_ = 0;
        bool confirm_pending_ = false;
        std::string pending_text_;
        std::atomic<bool> attended_{ false };
};

EndScreenSnapshot &get_end_screen_snapshot();

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_END_SCREEN_SNAPSHOT_H
