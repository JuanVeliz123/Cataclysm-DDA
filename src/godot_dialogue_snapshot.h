#pragma once
#ifndef CATA_SRC_GODOT_DIALOGUE_SNAPSHOT_H
#define CATA_SRC_GODOT_DIALOGUE_SNAPSHOT_H

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
 * NPC conversation as a Godot Control.
 *
 * The dialogue loop is not a screen with a state machine behind it, the way
 * crafting is -- it is one `do/while` inside `dialogue::opt_imgui` that reads an
 * action, mutates a couple of fields and loops. So this channel does not try to
 * take the screen over. It replaces exactly one thing: where the action comes
 * from. The loop publishes what it would have drawn, asks here for an action
 * string instead of asking the input context, and is otherwise untouched.
 *
 * That keeps the parts worth not reimplementing where they are: which responses
 * are selectable is re-verified after CONFIRM (a response can be shown and still
 * be refused), a hostile or helpless consequence puts up its own confirmation,
 * and the trade window hides the dialogue UI from underneath it.
 *
 * The panel may also answer with a row index, which means "select this and
 * confirm it" -- a click. It is applied as a selection followed by CONFIRM, so
 * it goes through the same re-verification a keyboard CONFIRM does rather than
 * around it.
 */
class DialogueSnapshot
{
    public:
        bool active() const;
        uint64_t generation() const;

        // --- Godot thread -------------------------------------------------
        godot::Dictionary copy_state() const;
        /// One of the action strings the DIALOGUE input context produces.
        void request_action( const std::string &action );
        /// Click a response: selects it and confirms in one step.
        void request_select( int index );
        void note_attended();
        bool attended() const;

        // --- game thread ---------------------------------------------------
        struct line {
            std::string text;
            /// CDDA colour name, e.g. "c_light_blue". Empty means the default.
            std::string color;
        };
        struct response {
            std::string text;
            std::string hotkey;
            std::string color;
        };

        void publish( const std::string &header, const std::vector<line> &history,
                      const std::vector<response> &responses, int selected );
        void clear();

        /**
         * Block until the panel asks for something.
         *
         * @param select out: the response index to confirm, or -1 for a plain
         *               action.
         * @return the action string, or "" when no panel attended -- the caller
         *         must then drive this turn from its own UI.
         */
        std::string next_action( int &select );

    private:
        mutable std::mutex mutex_;
        bool active_ = false;
        std::string header_;
        std::vector<line> history_;
        std::vector<response> responses_;
        int selected_ = 0;
        uint64_t generation_ = 0;
        std::string pending_action_;
        std::atomic<int> pending_select_{ -1 };
        std::atomic<bool> attended_{ false };
};

DialogueSnapshot &get_dialogue_snapshot();

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_DIALOGUE_SNAPSHOT_H
