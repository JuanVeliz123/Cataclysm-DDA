#include "godot_popup_snapshot.h"

#if defined(GODOT)

#include "godot_backend.h"
#include "godot_input_bridge.h"
#include "input_enums.h"
#include "output.h"
#include "popup.h"

#include <chrono>
#include <optional>
#include <thread>

#include <godot_cpp/variant/array.hpp>

namespace godot_backend
{

namespace
{

PopupSnapshot g_popup_snapshot;

godot::String gs( const std::string &s )
{
    return godot::String::utf8( s.c_str() );
}

} // namespace

PopupSnapshot &get_popup_snapshot()
{
    return g_popup_snapshot;
}

void PopupSnapshot::bump()
{
    ++generation_;
}

bool PopupSnapshot::active() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    if( prompt_active_ ) {
        return true;
    }
    for( const notice &n : notices_ ) {
        if( !n.text.empty() ) {
            return true;
        }
    }
    return false;
}

uint64_t PopupSnapshot::generation() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return generation_;
}

godot::Dictionary PopupSnapshot::copy_state() const
{
    const_cast<PopupSnapshot *>( this )->note_attended();
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::Dictionary d;
    d["prompt_active"] = prompt_active_;
    d["text"] = gs( prompt_text_ );
    d["allow_cancel"] = allow_cancel_;
    d["text_entry"] = text_entry_;
    d["entry_title"] = gs( entry_title_ );
    d["entry_label"] = gs( entry_label_ );
    d["entry_initial"] = gs( entry_initial_ );
    d["entry_max_length"] = entry_max_length_;
    d["generation"] = static_cast<int64_t>( generation_ );
    godot::Array opts;
    opts.resize( static_cast<int64_t>( options_.size() ) );
    for( size_t i = 0; i < options_.size(); ++i ) {
        opts[static_cast<int64_t>( i )] = gs( options_[i] );
    }
    d["options"] = opts;
    // Topmost non-empty notice. A stack of progress messages on screen at once is
    // noise, and the innermost is the current one -- but a popup is constructed
    // before its message is set, so an empty one is a box with nothing in it.
    std::string shown;
    for( auto it = notices_.rbegin(); it != notices_.rend(); ++it ) {
        if( !it->text.empty() ) {
            shown = it->text;
            break;
        }
    }
    d["notice"] = gs( shown );
    d["notice_active"] = !shown.empty();
    return d;
}

void PopupSnapshot::answer( const int index )
{
    reply_index_.store( index, std::memory_order_relaxed );
    reply_.store( static_cast<int8_t>( reply::chosen ), std::memory_order_release );
}

void PopupSnapshot::answer_text( const std::string &text )
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        reply_text_ = text;
    }
    reply_.store( static_cast<int8_t>( reply::chosen ), std::memory_order_release );
}

std::string PopupSnapshot::taken_text() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return reply_text_;
}

void PopupSnapshot::cancel()
{
    reply_.store( static_cast<int8_t>( reply::cancelled ), std::memory_order_release );
}

void PopupSnapshot::note_attended()
{
    attended_.store( true, std::memory_order_relaxed );
}

bool PopupSnapshot::attended() const
{
    return attended_.load( std::memory_order_relaxed );
}

uint64_t PopupSnapshot::push_notice( const std::string &text )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    const uint64_t handle = next_handle_++;
    notices_.push_back( { handle, remove_color_tags( text ) } );
    bump();
    return handle;
}

void PopupSnapshot::update_notice( const uint64_t handle, const std::string &text )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    for( notice &n : notices_ ) {
        if( n.handle == handle ) {
            const std::string plain = remove_color_tags( text );
            if( n.text != plain ) {
                n.text = plain;
                bump();
            }
            return;
        }
    }
}

void PopupSnapshot::retire_notice( const uint64_t handle )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    for( auto it = notices_.begin(); it != notices_.end(); ++it ) {
        if( it->handle == handle ) {
            // Erase by handle rather than popping the back: popups nest, and an
            // inner one going away must not take an outer one with it.
            notices_.erase( it );
            bump();
            return;
        }
    }
}

void PopupSnapshot::publish_prompt( const std::string &text,
                                    const std::vector<std::string> &options,
                                    const bool allow_cancel )
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        prompt_active_ = true;
        text_entry_ = false;
        prompt_text_ = remove_color_tags( text );
        options_ = options;
        allow_cancel_ = allow_cancel;
        bump();
    }
    reply_index_.store( -1, std::memory_order_relaxed );
    reply_.store( static_cast<int8_t>( reply::pending ), std::memory_order_release );
    attended_.store( false, std::memory_order_relaxed );
}

void PopupSnapshot::publish_text_prompt( const std::string &title,
                                        const std::string &description,
                                        const std::string &label,
                                        const std::string &initial,
                                        const int max_length )
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        prompt_active_ = true;
        text_entry_ = true;
        entry_title_ = remove_color_tags( title );
        prompt_text_ = remove_color_tags( description );
        entry_label_ = remove_color_tags( label );
        entry_initial_ = initial;
        entry_max_length_ = max_length;
        options_.clear();
        allow_cancel_ = true;
        reply_text_.clear();
        bump();
    }
    reply_index_.store( -1, std::memory_order_relaxed );
    reply_.store( static_cast<int8_t>( reply::pending ), std::memory_order_release );
    attended_.store( false, std::memory_order_relaxed );
}

void PopupSnapshot::clear_prompt()
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        prompt_active_ = false;
        text_entry_ = false;
        prompt_text_.clear();
        entry_title_.clear();
        entry_label_.clear();
        entry_initial_.clear();
        options_.clear();
        bump();
    }
    reply_index_.store( -1, std::memory_order_relaxed );
    reply_.store( static_cast<int8_t>( reply::pending ), std::memory_order_release );
    attended_.store( false, std::memory_order_relaxed );
}

PopupSnapshot::reply PopupSnapshot::take_reply( int &index )
{
    const reply r = static_cast<reply>( reply_.load( std::memory_order_acquire ) );
    if( r == reply::chosen ) {
        index = reply_index_.load( std::memory_order_relaxed );
    }
    return r;
}

bool run_anykey_popup_in_godot( const std::string &message, input_event &evt )
{
    PopupSnapshot &snap = get_popup_snapshot();
    const uint64_t handle = snap.push_notice( message );

    // The notice is display-only, so nothing here answers it. The key arrives on
    // the ordinary input path: the panel forwards the raw Godot event, the bridge
    // translates it, and this drains the result. Nothing else is draining the
    // bridge while the caller is parked in here.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 1500 );
    bool got = false;
    while( true ) {
        if( is_shutdown_requested() ) {
            break;
        }
        if( !snap.attended() && std::chrono::steady_clock::now() > deadline ) {
            snap.retire_notice( handle );
            return false;
        }
        const std::optional<input_event> next = get_input_bridge().pop_event();
        if( next && ( next->type == input_event_t::keyboard_char ||
                      next->type == input_event_t::keyboard_code ) &&
            !next->sequence.empty() ) {
            evt = *next;
            got = true;
            break;
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 4 ) );
    }
    snap.retire_notice( handle );
    return got;
}

bool run_popup_in_godot( const query_popup &popup, std::string &chosen_action )
{
    const std::vector<std::pair<std::string, std::string>> opts = popup.option_descriptions();
    if( opts.empty() ) {
        // No buttons means the caller is driving this some other way (anykey, or
        // a bare message). Leave those to the legacy path.
        return false;
    }

    PopupSnapshot &snap = get_popup_snapshot();
    std::vector<std::string> labels;
    labels.reserve( opts.size() );
    for( const std::pair<std::string, std::string> &o : opts ) {
        labels.push_back( remove_color_tags( o.second ) );
    }
    snap.publish_prompt( popup.get_message(), labels, popup.cancel_allowed() );

    // Same two rules as the uilist takeover, both learned the hard way: check
    // shutdown before anything else, and give up if no panel is attending rather
    // than waiting on an answer that will never come.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 1500 );
    int index = -1;
    while( true ) {
        if( is_shutdown_requested() ) {
            snap.clear_prompt();
            chosen_action = "QUIT";
            return true;
        }
        if( !snap.attended() && std::chrono::steady_clock::now() > deadline ) {
            snap.clear_prompt();
            return false;
        }
        const PopupSnapshot::reply r = snap.take_reply( index );
        if( r == PopupSnapshot::reply::cancelled ) {
            snap.clear_prompt();
            chosen_action = "QUIT";
            return true;
        }
        if( r == PopupSnapshot::reply::chosen ) {
            break;
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 4 ) );
    }

    snap.clear_prompt();
    if( index < 0 || index >= static_cast<int>( opts.size() ) ) {
        chosen_action = "QUIT";
        return true;
    }
    chosen_action = opts[index].first;
    return true;
}

bool run_text_prompt_in_godot( const std::string &title, const std::string &description,
                               const std::string &label, const int max_length,
                               std::string &value, bool &cancelled )
{
    PopupSnapshot &snap = get_popup_snapshot();
    snap.publish_text_prompt( title, description, label, value, max_length );

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 1500 );
    int unused = -1;
    while( true ) {
        if( is_shutdown_requested() ) {
            snap.clear_prompt();
            cancelled = true;
            return true;
        }
        if( !snap.attended() && std::chrono::steady_clock::now() > deadline ) {
            snap.clear_prompt();
            return false;
        }
        const PopupSnapshot::reply r = snap.take_reply( unused );
        if( r == PopupSnapshot::reply::cancelled ) {
            snap.clear_prompt();
            cancelled = true;
            return true;
        }
        if( r == PopupSnapshot::reply::chosen ) {
            value = snap.taken_text();
            snap.clear_prompt();
            cancelled = false;
            return true;
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 4 ) );
    }
}

} // namespace godot_backend

#endif // GODOT
