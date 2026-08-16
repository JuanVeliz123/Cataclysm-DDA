#pragma once
#ifndef CATA_SRC_GODOT_POPUP_SNAPSHOT_H
#define CATA_SRC_GODOT_POPUP_SNAPSHOT_H

#if defined(GODOT)

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <godot_cpp/variant/dictionary.hpp>

class query_popup;
struct input_event;

namespace godot_backend
{

/**
 * `query_popup` rendered as a Godot Control.
 *
 * Two shapes share this channel, because they are the same class:
 *
 * - A **prompt**: query() blocks until the player picks one of its options.
 *   Same contract as the uilist takeover -- publish, wait, return the answer.
 * - A **notice**: a `static_popup` that only displays. It takes no input and
 *   answers nothing; it appears when the object is constructed and goes when it
 *   is destroyed. The "Saving game, this may take a while." popup is one of
 *   these, which is why it is worth getting right: it is on screen during every
 *   autosave.
 *
 * A notice is a *stack*, not a single slot -- popups nest, and a nested one
 * must not erase the one underneath when it goes.
 */
class PopupSnapshot
{
    public:
        /// Whether anything should be on screen.
        bool active() const;
        uint64_t generation() const;
        godot::Dictionary copy_state() const;

        // --- Godot thread -------------------------------------------------
        /// Choose option @p index of the current prompt.
        void answer( int index );
        /// Submit the typed value of a text prompt.
        void answer_text( const std::string &text );
        /// Dismiss the prompt, where it allows that.
        void cancel();
        /// Proof a panel is rendering this; see the uilist snapshot for why the
        /// game thread cannot safely wait without it.
        void note_attended();
        bool attended() const;

        // --- game thread ---------------------------------------------------
        /// Push a display-only popup. Returns a handle for @ref retire_notice.
        uint64_t push_notice( const std::string &text );
        void update_notice( uint64_t handle, const std::string &text );
        void retire_notice( uint64_t handle );

        void publish_prompt( const std::string &text,
                             const std::vector<std::string> &options,
                             bool allow_cancel );
        /// A single-line text entry: same modal slot as a prompt, but the answer
        /// is typed rather than chosen.
        void publish_text_prompt( const std::string &title, const std::string &description,
                                  const std::string &label, const std::string &initial,
                                  int max_length );
        void clear_prompt();
        enum class reply : int8_t { pending, chosen, cancelled };
        reply take_reply( int &index );
        /// The submitted text, valid once take_reply reports `chosen`.
        std::string taken_text() const;

    private:
        void bump();

        struct notice {
            uint64_t handle = 0;
            std::string text;
        };

        mutable std::mutex mutex_;
        std::vector<notice> notices_;
        uint64_t next_handle_ = 1;

        bool prompt_active_ = false;
        std::string prompt_text_;
        std::vector<std::string> options_;
        bool allow_cancel_ = false;
        // Text-entry variant of the prompt slot.
        bool text_entry_ = false;
        std::string entry_title_;
        std::string entry_label_;
        std::string entry_initial_;
        int entry_max_length_ = 0;
        std::string reply_text_;

        uint64_t generation_ = 0;
        std::atomic<int> reply_index_{ -1 };
        std::atomic<int8_t> reply_{ static_cast<int8_t>( reply::pending ) };
        std::atomic<bool> attended_{ false };
};

PopupSnapshot &get_popup_snapshot();

/**
 * Run @p popup's prompt as a Godot panel.
 *
 * @param chosen_action out: the action string of the option picked, or "QUIT".
 * @return true when Godot handled it; false when the caller must use the
 *         legacy path (no options to offer, or no panel attending).
 */
bool run_popup_in_godot( const query_popup &popup, std::string &chosen_action );

/**
 * Show @p message and block until the player presses a key, returning it.
 *
 * This is the "press any key" popup, which is not a prompt: it has no options to
 * offer and its answer is the keypress itself. It matters most for rebinding a
 * key, where the event has to be the exact input_event the game will later
 * compare against -- describing the key and reconstructing it would produce a
 * binding that does not match when the player uses it. So the prompt goes up on
 * the notice channel and the key comes back through the ordinary input bridge,
 * which is the one place that knows how to translate a Godot key event.
 *
 * @return false when no panel attended, so the caller must use its own UI.
 */
bool run_anykey_popup_in_godot( const std::string &message, input_event &evt );

/**
 * Show a single-line text entry as a Godot panel and block until answered.
 *
 * @param value in/out: the initial text, and what the player typed.
 * @param cancelled out: true when dismissed without submitting.
 * @return false when no panel attended, so the caller must use its own UI.
 */
bool run_text_prompt_in_godot( const std::string &title, const std::string &description,
                               const std::string &label, int max_length,
                               std::string &value, bool &cancelled );

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_POPUP_SNAPSHOT_H
