#include "godot_dialogue_snapshot.h"

#if defined(GODOT)

#include "godot_backend.h"

#include <chrono>
#include <thread>
#include <utility>

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace godot_backend
{

namespace
{

DialogueSnapshot g_dialogue_snapshot;

godot::String gs( const std::string &s )
{
    return godot::String::utf8( s.c_str() );
}

} // namespace

DialogueSnapshot &get_dialogue_snapshot()
{
    return g_dialogue_snapshot;
}

bool DialogueSnapshot::active() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return active_;
}

uint64_t DialogueSnapshot::generation() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return generation_;
}

godot::Dictionary DialogueSnapshot::copy_state() const
{
    const_cast<DialogueSnapshot *>( this )->note_attended();
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::Dictionary d;
    d["active"] = active_;
    d["header"] = gs( header_ );
    d["selected"] = selected_;
    d["generation"] = static_cast<int64_t>( generation_ );

    godot::Array history;
    history.resize( static_cast<int64_t>( history_.size() ) );
    for( size_t i = 0; i < history_.size(); ++i ) {
        godot::Dictionary l;
        l["text"] = gs( history_[i].text );
        l["color"] = gs( history_[i].color );
        history[static_cast<int64_t>( i )] = l;
    }
    d["history"] = history;

    godot::Array responses;
    responses.resize( static_cast<int64_t>( responses_.size() ) );
    for( size_t i = 0; i < responses_.size(); ++i ) {
        godot::Dictionary r;
        r["text"] = gs( responses_[i].text );
        r["hotkey"] = gs( responses_[i].hotkey );
        r["color"] = gs( responses_[i].color );
        responses[static_cast<int64_t>( i )] = r;
    }
    d["responses"] = responses;
    return d;
}

void DialogueSnapshot::request_action( const std::string &action )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    // Last one wins rather than queueing: the loop consumes one action per
    // frame and a backlog would replay stale keys after the topic changed.
    pending_action_ = action;
}

void DialogueSnapshot::request_select( const int index )
{
    pending_select_.store( index, std::memory_order_relaxed );
}

void DialogueSnapshot::note_attended()
{
    attended_.store( true, std::memory_order_relaxed );
}

bool DialogueSnapshot::attended() const
{
    return attended_.load( std::memory_order_relaxed );
}

void DialogueSnapshot::publish( const std::string &header, const std::vector<line> &history,
                                const std::vector<response> &responses, const int selected )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    active_ = true;
    header_ = header;
    history_ = history;
    responses_ = responses;
    selected_ = selected;
    ++generation_;
}

void DialogueSnapshot::clear()
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        active_ = false;
        header_.clear();
        history_.clear();
        responses_.clear();
        selected_ = 0;
        pending_action_.clear();
        ++generation_;
    }
    pending_select_.store( -1, std::memory_order_relaxed );
    attended_.store( false, std::memory_order_relaxed );
}

std::string DialogueSnapshot::next_action( int &select )
{
    select = -1;
    // Same contract as the other takeovers, both learned the hard way: shutdown
    // wins, and a screen nothing is drawing is handed back rather than blocking
    // the game thread on an answer that will never come.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 1500 );
    while( true ) {
        if( is_shutdown_requested() ) {
            return "QUIT";
        }
        if( !attended() && std::chrono::steady_clock::now() > deadline ) {
            return std::string();
        }
        const int want = pending_select_.exchange( -1, std::memory_order_relaxed );
        if( want >= 0 ) {
            select = want;
            return "CONFIRM";
        }
        {
            std::lock_guard<std::mutex> lock( mutex_ );
            if( !pending_action_.empty() ) {
                std::string out;
                out.swap( pending_action_ );
                return out;
            }
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 4 ) );
    }
}

} // namespace godot_backend

#endif // GODOT
